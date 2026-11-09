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

#include <cstdint>

#ifdef FEATHERS_DIAGNOSTIC
#include <algorithm>
#include <cstdio>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#endif

// **IS ANYTHING SOUNDING ON THE SIDECHAIN?** It is SILENT when the host
// hands no buffer for the port, when it flags both channels constant at exactly 0 (`constant_mask`), or when a buffer it does hand
// has been exactly silent for `kHoldSeconds`; a pause shorter than that is still sounding. CLAP has no "connected" flag for an audio
// port, and a host feeds an unrouted port the same zeros it feeds a routed, silent one, so this does NOT claim to know which: it
// decides only what the card says ("sidechain silent") and whether Feathers may sleep. It never decides what the vocoder does —
// Vocode on always vocodes, and a silent sidechain gives a silent output.
namespace feathers::device {

class SidechainPresence {
public:
	static constexpr double kHoldSeconds = 1.0;

	/// FOR ONE HOST BLOCK: `left`/`right` the port's buffers (null when the host handed none), `constantZero` whether the host
	/// flagged both constant at 0. Answers whether the sidechain is sounding for this block. AUDIO THREAD; allocates nothing.
	bool update(const float* left, const float* right, uint32_t frames, bool constantZero, double sampleRate) noexcept {
		if (left == nullptr || right == nullptr || constantZero) {
			silentFrames_ = holdFrames(sampleRate);
			sounded_ = false;
			return sounding_ = false;
		}
		bool any = false;
		for (uint32_t i = 0; i < frames && !any; ++i) {
			any = left[i] != 0.0f || right[i] != 0.0f;
		}
		sounded_ = any;
		const uint64_t hold = holdFrames(sampleRate);
		silentFrames_ = any ? 0 : (silentFrames_ >= hold ? hold : silentFrames_ + frames);
		return sounding_ = silentFrames_ < hold;
	}

	[[nodiscard]] bool sounding() const noexcept { return sounding_; }

#ifdef FEATHERS_DIAGNOSTIC
	/// **WHAT THE HOST HANDED FOR THE PORT, ON A CHANGE**.
	/// One line to stderr (Bitwig's `~/.BitwigStudio/log/engine.log`), only when the shape moves: how many inputs the host
	/// passed, whether the port had a buffer, its channels, its constant mask, whether the last block sounded, and the verdict.
	/// A stack buffer and one `write(2)`, as SKLITTER's tripwire does, and a line budget. AUDIO THREAD; allocates nothing.
	void trace(const char* plugin, uint32_t inputs, bool buffer, uint32_t channels, uint64_t mask) noexcept {
		const uint64_t shape = (static_cast<uint64_t>(inputs) << 40) | (static_cast<uint64_t>(buffer) << 39)
		                       | (static_cast<uint64_t>(channels & 0xff) << 31) | ((mask & 0xff) << 23)
		                       | (static_cast<uint64_t>(sounded_) << 22) | (static_cast<uint64_t>(sounding_) << 21);
		if (shape == traced_ || traceLines_ <= 0) {
			return;
		}
		traced_ = shape;
		--traceLines_;
		char buf[256];
		const int len = std::snprintf(buf, sizeof buf,
		                              "[FEATHERS-SIDECHAIN] %s inputs=%u buffer=%d channels=%u constant_mask=0x%llx sounded=%d sounding=%d\n",
		                              plugin, inputs, buffer ? 1 : 0, channels, static_cast<unsigned long long>(mask),
		                              sounded_ ? 1 : 0, sounding_ ? 1 : 0);
		if (len > 0) {
			const auto n = std::min(static_cast<size_t>(len), sizeof buf - 1);
#if !defined(_WIN32)
			const ssize_t written = ::write(2, buf, n);
			(void)written;
#else
			std::fwrite(buf, 1, n, stderr);
#endif
		}
	}
#endif
	/// As if just switched on: nothing heard yet, so silent until something is.
	void reset(double sampleRate) noexcept {
		silentFrames_ = holdFrames(sampleRate);
		sounding_ = false;
	}

private:
	[[nodiscard]] static uint64_t holdFrames(double sampleRate) noexcept {
		return static_cast<uint64_t>(kHoldSeconds * (sampleRate > 0.0 ? sampleRate : 48000.0));
	}
	uint64_t silentFrames_ = UINT64_MAX / 2;
	bool sounding_ = false;
	bool sounded_ = false; ///< whether the last block carried a sample (the diagnostic trace reads it)
#ifdef FEATHERS_DIAGNOSTIC
	uint64_t traced_ = UINT64_MAX;
	int traceLines_ = 200;
#endif
};

} // namespace feathers::device
