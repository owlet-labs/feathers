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

#include "device/endpoint.h"

#include <atomic>
#include <cstdint>

// **WHAT THE ENGINE READS FOR ONE ROUTE, ASKED BY THE EDITOR WHILE A HAND SETS ITS AMOUNT** (backlog PU).
//
// The as-modulated readout (`ui/mod_readout.h`) prints `resolveRouted(base, Σ routeTerm)` with the amount
// under the hand. Everything in that sum except the amount is the ENGINE'S: the source's reading (the cell
// the engine's input points at — a toggle at "on" is 1, a held LFO its present output), every OTHER route into
// the destination, the host's modulation, and the span from the engine's own table. The editor cannot compute
// those; it can only be told them. So the editor names one (source, destination) pair, the audio thread
// answers it once a block from its own inputs (`RackEngine::probeRoute`), and the editor adds the one term
// it does know — its amount, which the engine will not have adopted until the next drain.
//
// ONE PAIR AT A TIME, because one hand sets one amount. A seqlock over atomics, so neither side blocks and a
// torn answer is retried rather than read.

namespace feathers::backplane {

/// ONE ROUTE'S SURROUNDINGS, AS THE ENGINE RESOLVES THEM. The offset the engine sums is
/// `before + routeTerm(amount, transfer, source, span) + after`, in that order — the inputs in the engine's
/// order with the watched one where it sits (or last, before the host's modulation, when it is not yet an
/// input), so the readout's sum is the engine's to the bit.
struct RouteProbe {
	/// The watched source's reading, from the cell the engine's input reads (the collapsed one — the readout is
	/// one number, not one per voice). A double because Feathers' core resolve reads a MACRO as a double
	/// (`ParamSet::resolved`); a device cell's float is carried exactly.
	double source = 0.0;
	/// The other routes the engine sums before the watched one.
	double before = 0.0;
	/// The other routes after it, then the host's modulation (the rack engine adds it after the routes).
	double after = 0.0;
	/// The destination's route span, from the engine's table (`routeSpanOf` at provision; `max − min` in `ParamSet`).
	double span = 0.0;
	/// FEATHERS' CORE ONLY: the host's own modulation of the parameter, which `ParamSet::resolved` adds to the BASE
	/// before the routes (`base + mod + Σ`). Zero from the rack engine, which folds its host modulation into `after`.
	double baseOffset = 0.0;
	/// FEATHERS' CORE ONLY: the watched source is a macro, which the core resolve reads as a double through
	/// `throughTransfer` rather than as a cell through `shapeTransfer` (`coreRouteTerm`).
	bool macro = false;
};

class RouteProbeChannel {
public:
	/// THE EDITOR NAMES THE PAIR IT IS WATCHING. UI thread.
	void watch(device::Source source, device::Destination destination) noexcept {
		watchSource_.store(pack(source.device, source.index), std::memory_order_relaxed);
		watchDestination_.store(pack(destination.device, destination.index), std::memory_order_relaxed);
	}
	/// AND STOPS — the audio thread then probes nothing.
	void unwatch() noexcept { watchSource_.store(kNone, std::memory_order_relaxed); }

	/// THE PAIR TO PROBE, or false for none. Audio thread.
	[[nodiscard]] bool watched(device::Source& source, device::Destination& destination) const noexcept {
		const uint64_t s = watchSource_.load(std::memory_order_relaxed);
		if (s == kNone) {
			return false;
		}
		const uint64_t d = watchDestination_.load(std::memory_order_relaxed);
		source = { hi(s), lo(s) };
		destination = { hi(d), lo(d) };
		return true;
	}

	/// THE ENGINE'S ANSWER FOR THAT PAIR. Audio thread.
	void publish(device::Source source, device::Destination destination, const RouteProbe& p) noexcept {
		const uint32_t s = seq_.load(std::memory_order_relaxed);
		seq_.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		source_.store(pack(source.device, source.index), std::memory_order_relaxed);
		destination_.store(pack(destination.device, destination.index), std::memory_order_relaxed);
		value_.store(p.source, std::memory_order_relaxed);
		baseOffset_.store(p.baseOffset, std::memory_order_relaxed);
		macro_.store(p.macro, std::memory_order_relaxed);
		before_.store(p.before, std::memory_order_relaxed);
		after_.store(p.after, std::memory_order_relaxed);
		span_.store(p.span, std::memory_order_relaxed);
		seq_.store(s + 2, std::memory_order_release);
	}

	/// THE LATEST ANSWER, IF IT IS ABOUT THIS PAIR — false for another pair, none yet, or a write in flight
	/// (the caller asks again next frame). UI thread.
	[[nodiscard]] bool read(device::Source source, device::Destination destination, RouteProbe& out) const noexcept {
		const uint32_t s1 = seq_.load(std::memory_order_acquire);
		if ((s1 & 1u) != 0 || s1 == 0) {
			return false;
		}
		const uint64_t src = source_.load(std::memory_order_relaxed);
		const uint64_t dst = destination_.load(std::memory_order_relaxed);
		RouteProbe p;
		p.source = value_.load(std::memory_order_relaxed);
		p.baseOffset = baseOffset_.load(std::memory_order_relaxed);
		p.macro = macro_.load(std::memory_order_relaxed);
		p.before = before_.load(std::memory_order_relaxed);
		p.after = after_.load(std::memory_order_relaxed);
		p.span = span_.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (seq_.load(std::memory_order_relaxed) != s1) {
			return false;
		}
		if (src != pack(source.device, source.index) || dst != pack(destination.device, destination.index)) {
			return false;
		}
		out = p;
		return true;
	}

private:
	static constexpr uint64_t kNone = ~uint64_t { 0 };
	static constexpr uint64_t pack(uint32_t a, uint32_t b) noexcept { return (uint64_t { a } << 32) | b; }
	static constexpr uint32_t hi(uint64_t v) noexcept { return static_cast<uint32_t>(v >> 32); }
	static constexpr uint32_t lo(uint64_t v) noexcept { return static_cast<uint32_t>(v); }

	std::atomic<uint64_t> watchSource_ { kNone };
	std::atomic<uint64_t> watchDestination_ { kNone };
	std::atomic<uint32_t> seq_ { 0 };
	std::atomic<uint64_t> source_ { kNone };
	std::atomic<uint64_t> destination_ { kNone };
	std::atomic<double> value_ { 0.0 };
	std::atomic<double> baseOffset_ { 0.0 };
	std::atomic<bool> macro_ { false };
	std::atomic<double> before_ { 0.0 };
	std::atomic<double> after_ { 0.0 };
	std::atomic<double> span_ { 0.0 };
};

} // namespace feathers::backplane
