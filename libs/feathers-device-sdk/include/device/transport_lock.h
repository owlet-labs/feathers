/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <cmath>

// IN THE DEVICE TIER, BESIDE `TransportClock`, because BOTH lanes lock to the song: the modulators
// (LFO, MSEG) and automod's LFO in the fx lane all had the same per-tick `position / window` and the
// same defect under a Rate route. `mod/modulator.h` aliases it for the modulator tier.

namespace feathers::device {

/// **HOW A FREE SHAPE FOLLOWS THE SONG: BY THE SONG'S MOVEMENT, ANCHORED TO ITS POSITION ONLY AT A JUMP.**
///
/// A Free shape on a playing transport used to be PLACED every tick at `position / window` — backlog DQ's
/// lock, so the same passage played twice gives the same shape. That is exact while the window holds
/// still and wrong the moment anything moves it: a route into Rate changes `window`, and the quotient
/// then moves by `turns × Δwindow / window`, which grows with the song. MEASURED (the symptom was
/// shapes retriggering after one segment): a Free MSEG whose Rate a 0.5 Hz sine moved by 5% ran smoothly
/// through its segments on a stopped transport, and from beat 64 of a playing one it read segments
/// 1, 2, 0, 2, 1, 0, 2 at 67 ms intervals — thrown back to the start mid-cycle over and over.
///
/// **SO THE POSITION IS ADVANCED BY THE TRANSPORT'S OWN STEP**, `Δposition / window` with the window as it
/// is NOW, which is what a rate is. While the window holds still the sum telescopes to exactly
/// `position / window`, so DQ's determinism is kept for every shape nothing is modulating; under a route
/// the shape is frequency-modulated rather than scrubbed. That is the chosen behaviour.
///
/// **IT RE-ANCHORS TO `position / window` ONLY WHERE THE SONG JUMPED** — the first tick of a lock (the
/// play press, or a switch into Free), a host loop wrapping back, a seek, a unit change (the host began or
/// stopped reporting a tempo): a step BACKWARDS, or forwards by more than `kJumpTicks` ticks. The window
/// is generous on purpose. A reader that samples the clock at a chunk's start rather than at its own tick
/// (automod, in ≤32-frame chunks) sees steps that wander by up to half a tick, and a host that repeats a
/// position sees a step of zero; neither is a jump. And whatever falls inside the window costs nothing
/// when unmodulated, because a step followed and a step re-anchored land in the same place while the
/// window holds still — the two differ only under a route, and a forward seek of four ticks under a route
/// is not a thing anybody can hear. Everything in one unit, which the caller names: beats against a beat
/// window, or seconds against a seconds one.
class TransportLock {
public:
	/// A forward step longer than this many ticks is a seek; see above for why the window is this wide.
	static constexpr double kJumpTicks = 4.0;

	/// The lock is not in force this tick: the next one that is will anchor — on the song again, so a Trig's
	/// shift (`restartAt`) ends with the lock it was made under.
	void release() noexcept {
		anchored_ = false;
		shift_ = 0.0;
	}

	/// **A TRIG RESTARTED THE SHAPE WHILE THE SONG HOLDS IT** (backlog MV). Called after this tick's `follow`: the
	/// shape reads `turns` now and follows the song's step from there, so a phase reset on a Free shape is a reset
	/// rather than something the next tick's lock undoes. The shift outlives jumps (a host loop keeps the offset the
	/// player fired) and ends at `release`.
	void restartAt(double turns) noexcept { shift_ = turns_ - turns; }

	/// The shape's position in turns (whole cycles of the window) for a song standing at `position`,
	/// given the window and the length of one tick in the same unit. `unit` names that unit, so a
	/// change of road re-anchors rather than subtracting seconds from beats.
	double follow(double position, double window, double tickLength, int unit) noexcept {
		const double step = position - position_;
		const bool jumped = !anchored_ || unit != unit_ || !(step >= 0.0)
		                    || step > kJumpTicks * tickLength;
		if (jumped) {
			turns_ = position / window;
		}
		else {
			turns_ += step / window;
		}
		position_ = position;
		unit_ = unit;
		anchored_ = true;
		return turns_ - shift_;
	}

private:
	double turns_ = 0.0;
	double shift_ = 0.0;
	double position_ = 0.0;
	int unit_ = 0;
	bool anchored_ = false;
};

} // namespace feathers::device
