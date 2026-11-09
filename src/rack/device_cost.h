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
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace feathers::rack {

/// THE MONOTONIC NANOSECOND CLOCK, for what is not per sample.
[[nodiscard]] inline uint64_t costClockNs() noexcept {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// **THE CYCLE COUNTER THE TIMING READS — NOT `steady_clock`, MEASURED (2026-11-04).** On this host the kernel's
/// clocksource is HPET, so `steady_clock::now()` is a real syscall at 1.19 us a read (TSC: 8.4 ns), and two reads per
/// device per control tick put ~43 us on a four-device block. The TSC (x86, constant and nonstop here) and the generic
/// timer (arm64, `cntvct_el0`) are unprivileged register reads; the recorder calibrates them against `steady_clock` on
/// the main thread. Other targets fall back to the nanosecond clock (one tick = 1 ns).
[[nodiscard]] inline uint64_t costTicks() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
	return __rdtsc();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
	uint64_t v = 0;
	asm volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
#else
	return costClockNs();
#endif
}

/// **WHERE A BLOCK'S TIME WENT OUTSIDE THE DEVICES** (backlog SR, second pass): the stages a `StageScope` names. Each
/// counts its OWN time: a device that runs inside it, and a stage nested inside it, are taken out (`CostTree`).
enum class Stage : uint8_t {
	Events,  ///< the host's events and the editor's, consumed (feathers.clap's `consumeEventsFrom`, `consumeEditorEvents`)
	Inputs,  ///< the block's non-event inputs read: transport, bridge handles, editor lanes
	Params,  ///< parameters resolved and applied (`Synth::applyParams*`, pitches)
	Voices,  ///< the voices' own render: oscillators, stacks, the voice gain (per-voice devices taken out)
	Bus,     ///< the lane's bus pass around its devices (the devices taken out)
	Mods,    ///< the modulators' control tick (`Synth::tickEnvelopes`, `RackEngine::tickModulators`)
	Rebake,  ///< the character bank: `updateCharacter`, its builds and its advance
	Resolve, ///< an engine's block start: rows resolved, inputs rebuilt, the latency domains' arrival walk
	Publish, ///< what the editor reads, published at the block's end
	Count
};
inline constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
inline constexpr const char* kStageNames[kStageCount] = { "events", "inputs", "params", "voices", "bus",
	                                                      "mods",   "rebake", "resolve", "publish" };

/// ONE TIMED DEVICE IN ONE BLOCK, at any depth: a device in the lane (depth 0), or one inside a container's chain under
/// it. Its `ticks` include everything it holds.
struct CostNode {
	uint64_t ticks = 0;
	const char* tag = nullptr;  ///< the device's four-letter tag
	const char* lane = nullptr; ///< the inner chain's name under its container ("post", "chain"), or null
	uint16_t parent = 0;        ///< `CostTree::kRoot` at depth 0
	uint16_t slot = 0;          ///< its slot in its own engine
	int8_t laneIndex = -1;      ///< which of a container's numbered chains (a Rack's), or -1
	uint8_t depth = 0;
};

/// **WHAT EVERY DEVICE AND STAGE COST THE BLOCK IN FLIGHT, AS A TREE** (the spike recorder, backlog SR). Every
/// `RackEngine` on the thread times its devices into the tree bound there (`bindCostTree`) — the lane's own engine, and
/// the engines inside a Rack, a Chain, SKLITTER's post and grain chains, Resonate's lanes and every device's site — so a
/// device inside a container is named by its PATH, under the container that holds it. Fixed size: AUDIO THREAD ONLY,
/// no allocation; a block that times more than `kMaxNodes` devices loses the rest (counted in `lost()`).
class CostTree {
public:
	static constexpr size_t kMaxNodes = 256;
	static constexpr uint16_t kRoot = 0xffff; ///< the cursor at the top: no device open
	static constexpr uint16_t kLost = 0xfffe; ///< the cursor under a device the tree had no room for
	static constexpr size_t kRootSlots = 512; ///< depth-0 slots found by index; past this, by a scan
	static constexpr uint8_t kNoStage = 0xff;

	/// Empties it for the next block: only the nodes used are touched.
	void clear() noexcept {
		for (size_t i = 0; i < count_; ++i) {
			if (nodes_[i].depth == 0 && nodes_[i].slot < kRootSlots) {
				rootIndex_[nodes_[i].slot] = kRoot;
			}
		}
		count_ = 0;
		lost_ = 0;
		cursor_ = kRoot;
		openStage_ = kNoStage;
		stages_.fill(0);
	}

	/// A DEVICE STARTS: its node under the cursor — found, or made — becomes the cursor. Returns the node, or `kLost`.
	uint16_t enter(size_t slot, const char* tag, const char* lane, int laneIndex) noexcept {
		if (cursor_ == kLost) {
			return kLost;
		}
		const auto index = static_cast<int8_t>(std::clamp(laneIndex, -1, 127));
		uint16_t at = kLost;
		if (cursor_ == kRoot && slot < kRootSlots) {
			const uint16_t i = rootIndex_[slot];
			if (i != kRoot && nodes_[i].tag == tag) {
				at = i;
			}
		}
		else {
			for (size_t i = 0; i < count_; ++i) {
				const CostNode& n = nodes_[i];
				if (n.parent == cursor_ && n.slot == slot && n.tag == tag && n.lane == lane && n.laneIndex == index) {
					at = static_cast<uint16_t>(i);
					break;
				}
			}
		}
		if (at == kLost) {
			if (count_ >= kMaxNodes) {
				++lost_;
				cursor_ = kLost;
				return kLost;
			}
			at = static_cast<uint16_t>(count_++);
			CostNode& n = nodes_[at];
			n.ticks = 0;
			n.tag = tag;
			n.lane = lane;
			n.parent = cursor_;
			n.slot = static_cast<uint16_t>(slot);
			n.laneIndex = index;
			n.depth = static_cast<uint8_t>(cursor_ == kRoot ? 0 : nodes_[cursor_].depth + 1);
			if (cursor_ == kRoot && slot < kRootSlots) {
				rootIndex_[slot] = at;
			}
		}
		cursor_ = at;
		return at;
	}

	/// A DEVICE ENDS: its time added, the cursor back to `parent`, and — at depth 0 — taken out of the open stage.
	void leave(uint16_t node, uint16_t parent, uint64_t ticks) noexcept {
		if (node < kMaxNodes) {
			nodes_[node].ticks += ticks;
			if (nodes_[node].depth == 0 && openStage_ != kNoStage) {
				stages_[openStage_] -= ticks; // unsigned on purpose: the stage's own close adds it back in
			}
		}
		cursor_ = parent;
	}

	[[nodiscard]] uint16_t cursor() const noexcept { return cursor_; }
	[[nodiscard]] bool atRoot() const noexcept { return cursor_ == kRoot; }

	/// A STAGE STARTS (at the top only — a stage inside a device is that device's time). Returns the stage it nests in.
	uint8_t openStage(Stage s) noexcept {
		const uint8_t was = openStage_;
		openStage_ = static_cast<uint8_t>(s);
		return was;
	}
	/// A STAGE ENDS: its time added to it and taken out of the one it nested in.
	void closeStage(Stage s, uint8_t outer, uint64_t ticks) noexcept {
		stages_[static_cast<size_t>(s)] += ticks;
		if (outer != kNoStage) {
			stages_[outer] -= ticks;
		}
		openStage_ = outer;
	}

	[[nodiscard]] size_t count() const noexcept { return count_; }
	[[nodiscard]] const CostNode& node(size_t i) const noexcept { return nodes_[i]; }
	[[nodiscard]] uint64_t stageTicks(size_t s) const noexcept { return stages_[s]; }
	[[nodiscard]] uint32_t lost() const noexcept { return lost_; }

private:
	std::array<CostNode, kMaxNodes> nodes_ {};
	std::array<uint16_t, kRootSlots> rootIndex_ = [] {
		std::array<uint16_t, kRootSlots> a {};
		a.fill(kRoot);
		return a;
	}();
	std::array<uint64_t, kStageCount> stages_ {};
	size_t count_ = 0;
	uint32_t lost_ = 0;
	uint16_t cursor_ = kRoot;
	uint8_t openStage_ = kNoStage;
};

/// **HOW MANY RECORDERS EXIST IN THIS MODULE** — the gate in front of the thread-local below. A ship build never
/// makes one, so every `CostScope` and `StageScope` there costs one relaxed load and a branch, never a TLS lookup.
inline std::atomic<int> gCostTreesLive { 0 };
/// The tree this thread's block is timed into, or null (`bindCostTree`).
inline thread_local CostTree* tCostTree = nullptr;

/// **THE TREE THE BLOCK IN FLIGHT IS TIMED INTO, OR NULL.** Ambient — read from the thread, not handed down — because
/// it must reach engines no host can name: the ones inside a container's storage, made and dropped by the patch. A
/// container needs no line of its own to be broken out; its engine already times its devices.
[[nodiscard]] inline CostTree* ambientCostTree() noexcept {
	return gCostTreesLive.load(std::memory_order_relaxed) != 0 ? tCostTree : nullptr;
}
/// AUDIO THREAD: bind `tree` for this thread's block (null unbinds); returns what was bound.
inline CostTree* bindCostTree(CostTree* tree) noexcept {
	CostTree* was = tCostTree;
	tCostTree = tree;
	return was;
}

/// ONE DEVICE'S PASS, TIMED into the ambient tree under whatever device is open (nothing at all when none is bound,
/// not even the clock). `lane`/`laneIndex` name the inner chain its engine is (`RackEngine::setCostLane`).
class CostScope {
public:
	CostScope(size_t slot, const char* tag, const char* lane, int laneIndex) noexcept : tree_(ambientCostTree()) {
		if (tree_ != nullptr) {
			parent_ = tree_->cursor();
			node_ = tree_->enter(slot, tag, lane, laneIndex);
			start_ = costTicks();
		}
	}
	~CostScope() {
		if (tree_ != nullptr) {
			tree_->leave(node_, parent_, costTicks() - start_);
		}
	}
	CostScope(const CostScope&) = delete;
	CostScope& operator=(const CostScope&) = delete;

private:
	CostTree* tree_;
	uint64_t start_ = 0;
	uint16_t node_ = CostTree::kLost;
	uint16_t parent_ = CostTree::kRoot;
};

/// ONE STAGE, TIMED: only at the top of the tree (inside a device it is that device's time, and counted there).
class StageScope {
public:
	explicit StageScope(Stage stage) noexcept : tree_(ambientCostTree()), stage_(stage) {
		if (tree_ != nullptr && !tree_->atRoot()) {
			tree_ = nullptr;
		}
		if (tree_ != nullptr) {
			outer_ = tree_->openStage(stage);
			start_ = costTicks();
		}
	}
	~StageScope() {
		if (tree_ != nullptr) {
			tree_->closeStage(stage_, outer_, costTicks() - start_);
		}
	}
	StageScope(const StageScope&) = delete;
	StageScope& operator=(const StageScope&) = delete;

private:
	CostTree* tree_;
	Stage stage_;
	uint8_t outer_ = CostTree::kNoStage;
	uint64_t start_ = 0;
};

} // namespace feathers::rack
