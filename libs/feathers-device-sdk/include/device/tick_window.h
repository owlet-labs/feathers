/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

// A SOURCE'S CONTROL TICK, AS SAMPLES (backlog FK).
//
// **WHY THIS EXISTS.** Modulators run once per control tick (64 samples) and a reader that took their one
// value held it until the next tick: every routed destination was a staircase, audible as zipper noise on a
// gain (a route from an envelope to level left control-rate noise on the attack).
// Smoothing in each reader is state per reader, reset on every note start, steal and device swap; so the
// SOURCE publishes its tick as samples instead, and a reader indexes the sample it is rendering.
//
// **THE LAYOUT.** `kTickWindow` values — what an output is at each sample of one tick — then one flag float,
// 1 when every value is the same. The flag lets a reader take the scalar road (the cost of before) while
// nothing moves, and lets a writer skip rewriting a window already flat at the value it would write.
//
// **WHAT A WINDOW HOLDS TODAY** is the straight line from the last tick's value to this one's, because every
// source is a block-rate kernel (sst's envelope fills its own block the same way). A source that computes
// inside its tick can write its samples here instead, and every reader gets them with nothing changed.

#include <cstddef>

namespace feathers::device {

/// ONE CONTROL TICK — `mod::kBlockSamples` and `PuckMotion::kTickSamples`, asserted equal where both are visible.
inline constexpr int kTickWindow = 64;
/// Values, then the flat flag.
inline constexpr int kTickWindowStride = kTickWindow + 1;

[[nodiscard]] inline bool tickWindowFlat(const float* window) noexcept { return window[kTickWindow] != 0.0f; }

/// THE TICK FROM `prev` TO `next`, a straight line — which is what every source today produces between
/// ticks (sst's envelope fills its block the same way). A tick that does not move writes nothing when the
/// window is already flat there.
inline void fillTickWindowRamp(float* window, float prev, float next) noexcept {
	if (next == prev) {
		if (tickWindowFlat(window) && window[kTickWindow - 1] == next) {
			return;
		}
		for (int i = 0; i < kTickWindow; ++i) {
			window[i] = next;
		}
		window[kTickWindow] = 1.0f;
		return;
	}
	const float step = (next - prev) / static_cast<float>(kTickWindow);
	for (int i = 0; i < kTickWindow; ++i) {
		window[i] = prev + step * static_cast<float>(i + 1);
	}
	window[kTickWindow] = 0.0f;
}

} // namespace feathers::device
