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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// THE EDITOR'S FRAME TIMES (the UI on macOS felt sluggish while the host showed low CPU, so the audio
// thread is fine and the question is the drawing). Three series, each a rolling window of the last `kWindow` samples:
//
// - DRAW: the time inside visage's `ApplicationEditor::drawWindow` that drew a frame — stale frames' draw() calls plus
//   `Canvas::submit` (the CPU side; the GPU's own time is not in it).
// - WAIT: the time `drawWindow` spent in `Window::waitForFrame` before it, the swap chain's back-pressure.
// - INTERVAL: from one presented frame's start to the next — the pacing the eye sees. A 60 Hz display reads ~16.7 ms.
// - TICK: the editor's heartbeat (`Client::tick`: parameter diff, state channel, the plugin's own beat).
//
// **ONLY A FEATHERS_DIAGNOSTIC BUILD RECORDS ANYTHING.** `PluginWindow` hangs the draw callback and times the heartbeat
// under `#ifdef FEATHERS_DIAGNOSTIC`, so a ship build has no callback (visage tests an empty std::function), no clock read
// and no record. The type itself compiles everywhere so its arithmetic is testable in any build.
//
// PROCESS-GLOBAL (`frameTiming()`): with two editors open their samples share the window. MAIN THREAD ONLY.

namespace feathers::ui {

class FrameTiming {
public:
	static constexpr size_t kWindow = 240; ///< about four seconds at 60 Hz

	struct Stats {
		double p50 = 0.0;
		double p95 = 0.0;
		double max = 0.0;
		size_t count = 0;
	};

	class Series {
	public:
		void add(double ms) noexcept {
			samples_[next_] = ms;
			next_ = (next_ + 1) % kWindow;
			count_ = std::min(count_ + 1, kWindow);
		}
		/// Nearest-rank percentiles over the window: p50 is the ceil(0.5 n)-th smallest, p95 the ceil(0.95 n)-th.
		[[nodiscard]] Stats stats() const noexcept {
			Stats s;
			s.count = count_;
			if (count_ == 0) {
				return s;
			}
			std::array<double, kWindow> sorted {};
			std::copy_n(samples_.begin(), count_, sorted.begin());
			std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
			const auto rank = [&](double q) {
				// ceil, less a hair so 0.95 * 100 (95.000000000000014 in binary) is rank 95 and not 96.
				const size_t r = static_cast<size_t>(std::ceil(q * static_cast<double>(count_) - 1e-9));
				return sorted[std::clamp<size_t>(r, 1, count_) - 1];
			};
			s.p50 = rank(0.50);
			s.p95 = rank(0.95);
			s.max = sorted[count_ - 1];
			return s;
		}
		void clear() noexcept {
			next_ = 0;
			count_ = 0;
		}

	private:
		std::array<double, kWindow> samples_ {};
		size_t next_ = 0;
		size_t count_ = 0;
	};

	/// One `drawWindow`: `startMs` is a monotonic clock in ms at the draw's start, for the interval.
	void drawn(double startMs, double waitMs, double drawMs, bool presented) noexcept {
		if (!presented) {
			return; // nothing was stale: a no-op draw says nothing about a frame's cost
		}
		draw.add(drawMs);
		wait.add(waitMs);
		if (lastPresentedMs_ >= 0.0) {
			interval.add(startMs - lastPresentedMs_);
		}
		lastPresentedMs_ = startMs;
	}
	void ticked(double ms) noexcept { tick.add(ms); }
	/// A pause in drawing (the editor hidden or closed) is not an interval.
	void restartIntervals() noexcept { lastPresentedMs_ = -1.0; }

	/// DRAW COST and WAITING FOR THE NEXT FRAME apart: "draw p50 1.2 ms, ...; wait p50 0.1 ms, ...; interval ...; tick ..."
	[[nodiscard]] std::string summary() const {
		const Stats d = draw.stats();
		const Stats i = interval.stats();
		const Stats t = tick.stats();
		const Stats w = wait.stats();
		char text[256];
		std::snprintf(text, sizeof text,
		              "draw p50 %.1f ms, p95 %.1f ms, max %.1f ms; wait p50 %.1f ms, p95 %.1f ms; interval p50 %.1f ms, "
		              "p95 %.1f ms, max %.1f ms; tick p50 %.1f ms, p95 %.1f ms (n=%zu)",
		              d.p50, d.p95, d.max, w.p50, w.p95, i.p50, i.p95, i.max, t.p50, t.p95, d.count);
		return text;
	}

	/// A SESSION COUNTER (a restart made the UI seem faster, so growth over a session is the
	/// suspect): every `PluginWindow::create`, first build or reopen.
	uint64_t editorOpens = 0;

	Series draw;
	Series wait;
	Series interval;
	Series tick;

private:
	double lastPresentedMs_ = -1.0;
};

/// The process's one record. Main thread.
inline FrameTiming& frameTiming() {
	static FrameTiming timing;
	return timing;
}

} // namespace feathers::ui
