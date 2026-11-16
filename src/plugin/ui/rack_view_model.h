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

#include "plugin/ui/rack_strike_hub.h"
#include "device/modulatable.h"
#include "plugin/ui/frame_watchers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// THE RACK'S TWO NEW VIEWS, AGAINST A MODEL (src/plugin/ui/rack_views.h draws them).
//
// A rack is shown as STRIPS (the chains laid side by side, as before), as TABS (a bar of chain names with one chain in front) or as
// PADS (a 4x4 play surface over sixteen keys, with a strip of eight pages). Which chain a note reaches is its ROUTING: Parallel (all),
// Select (the one playing chain) or Keyed (the chain whose key it is). The widgets read and write the rack only through `RackViewModel`,
// so they are built and tested before the engine half exists: the Rack card hands them an adapter over the rack's own record, a test
// hands them a fake.

namespace feathers::ui {

enum class RackViewKind { Strips, Tabs, Pads };
enum class RackRoutingKind { Parallel, Select, Keyed };

/// A chain that answers every key (rather than one).
inline constexpr int kChainAnyKey = -1;
/// The most tabs a bar shows, and the geometry of the pad view: sixteen pads a page, eight pages, the whole of the keyboard.
inline constexpr size_t kMaxRackTabs = 8;
inline constexpr int kPadsPerPage = 16;
inline constexpr int kPadPages = 8;
inline constexpr size_t kNoChain = static_cast<size_t>(-1);

/// WHAT A CHAIN IS, as the views need it: whether it exists (only existing chains get a tab or a lit pad), its name, the key it answers.
struct RackChainView {
	bool exists = false;
	std::string name;
	int key = kChainAnyKey; ///< 0..127, or `kChainAnyKey`
	int chokeGroup = 0;     ///< 0 none
};

/// THE KEYS STRUCK since the last look, one bit per key (key k is bit k % 64 of word k / 64).
using RackKeyMask = std::array<uint64_t, 2>;

[[nodiscard]] constexpr bool maskHas(const RackKeyMask& mask, int key) noexcept {
	return key >= 0 && key < 128 && ((mask[static_cast<size_t>(key) >> 6] >> (static_cast<unsigned>(key) & 63u)) & 1u) != 0u;
}
constexpr void maskSet(RackKeyMask& mask, int key) noexcept {
	if (key >= 0 && key < 128) {
		mask[static_cast<size_t>(key) >> 6] |= uint64_t { 1 } << (static_cast<unsigned>(key) & 63u);
	}
}
[[nodiscard]] constexpr bool maskAny(const RackKeyMask& mask) noexcept {
	return (mask[0] | mask[1]) != 0u;
}

/// THE PAD ORDER: left to right, then bottom to top, so the lowest key of a page is the bottom-left pad. `index` is 0..15 within the page.
[[nodiscard]] constexpr int padKey(int page, int index) noexcept {
	return page * kPadsPerPage + index;
}
[[nodiscard]] constexpr int padColumn(int index) noexcept {
	return index % 4;
}
/// The row counted from the BOTTOM (0 is the bottom row); the drawing flips it.
[[nodiscard]] constexpr int padRowFromBottom(int index) noexcept {
	return index / 4;
}

/// WHAT THE VIEWS READ AND WRITE. Every write goes to the rack's own state (a chain's view and the pads' page are editor state; the
/// playing chain and the kit level are real parameters, written as gestures by the adapter).
class RackViewModel {
public:
	virtual ~RackViewModel() = default;

	[[nodiscard]] virtual RackViewKind view() const = 0;
	[[nodiscard]] virtual RackRoutingKind routing() const = 0;

	/// The chain slots, existing or not; `chain(slot).exists` says which are real.
	[[nodiscard]] virtual size_t chainSlots() const = 0;
	[[nodiscard]] virtual RackChainView chain(size_t slot) const = 0;

	/// THE CHAIN IN FRONT (Tabs) or SELECTED (Pads): editor state.
	[[nodiscard]] virtual size_t viewedChain() const = 0;
	virtual void viewChain(size_t slot) = 0;

	/// THE PLAYING CHAIN (Select routing only), a real parameter; `playingControl` is the control it is written through (declared on the
	/// bar's face for the window-superset rule), null where there is none.
	[[nodiscard]] virtual size_t playingChain() const = 0;
	virtual void setPlayingChain(size_t slot) = 0;
	[[nodiscard]] virtual device::Modulatable* playingControl() = 0;

	/// THE PADS' PAGE, 0..7.
	[[nodiscard]] virtual int page() const = 0;
	virtual void setPage(int page) = 0;

	/// WHETHER CHAIN `slot` IS OPEN INLINE (its whole chain shown), and whether the macro row is (editor state, collapsed by default).
	[[nodiscard]] virtual bool expanded(size_t slot) const = 0;
	virtual void setExpanded(size_t slot, bool expanded) = 0;
	[[nodiscard]] virtual bool macrosOpen() const = 0;
	virtual void setMacrosOpen(bool open) = 0;

	/// THE KEYS STRUCK SINCE THE LAST LOOK, cleared by the look (one consumer: `RackStrikePulse::poll`).
	[[nodiscard]] virtual RackKeyMask takeStruck() = 0;

	/// WHICH RACK THIS IS, for the strike hub: the host that holds it and its device id (null host: no hub, the pulse takes the mask itself).
	[[nodiscard]] virtual RackStrikeKey strikeKey() const { return {}; }

	/// FOLLOW PLAYED NOTE (Pads): whether a struck key that has a chain makes it the viewed one and turns to its page, and the control it
	/// is set through (null where there is none). View state: it changes nothing you hear.
	[[nodiscard]] virtual bool followPlayed() const { return false; }
	[[nodiscard]] virtual device::Modulatable* followControl() { return nullptr; }

	/// TRIGGERING A KEY FROM THE EDITOR (the preview road): true when it went. False where the host has no note road.
	[[nodiscard]] virtual bool canTrigger() const { return false; }
	virtual bool trigger(int /*key*/, bool /*on*/, float /*velocity*/) { return false; }

	/// THE KIT LEVEL, the one control of the pad view (a real parameter, null where the rack has none).
	[[nodiscard]] virtual device::Modulatable* kitLevel() = 0;
};

/// The chain whose key is `key` (the first, in slot order), or `kNoChain`: a pad is USED when this finds one.
[[nodiscard]] inline size_t chainForKey(const RackViewModel& model, int key) {
	for (size_t i = 0; i < model.chainSlots(); ++i) {
		const RackChainView c = model.chain(i);
		if (c.exists && c.key == key) {
			return i;
		}
	}
	return kNoChain;
}

/// The slots that get a tab: the existing chains, in order, at most `kMaxRackTabs`.
[[nodiscard]] inline std::vector<size_t> tabbedChains(const RackViewModel& model) {
	std::vector<size_t> out;
	for (size_t i = 0; i < model.chainSlots() && out.size() < kMaxRackTabs; ++i) {
		if (model.chain(i).exists) {
			out.push_back(i);
		}
	}
	return out;
}

/// THE EXTENTS OF `count` EQUAL PARTS OF `total` NATIVE PIXELS: the parts differ by at most one pixel and sum to `total` exactly, so a bar
/// of two tabs is two large ones and a bar of eight is eight small ones, never small tabs and a gap. Part i spans [edge(i), edge(i + 1)).
[[nodiscard]] constexpr int evenEdge(int total, size_t count, size_t i) noexcept {
	return count == 0 ? 0 : static_cast<int>((static_cast<long long>(total) * static_cast<long long>(i)) / static_cast<long long>(count));
}

/// THE STRIKE PULSE: which keys flash and how bright, shared by the pad grid and the page strip. ONE CONSUMER of the model's strike mask
/// (the mask clears on a look), so the owner calls `poll` once a tick and every watcher is repainted from here. The flash decays over
/// `kDecaySeconds`; under `FEATHERS_REDUCED_MOTION` a struck key is one steady highlight until the next look, with no animation.
class RackStrikePulse {
public:
	static constexpr double kDecaySeconds = 0.15;

	RackStrikePulse();
	RackStrikePulse(const RackStrikePulse&) = delete;
	RackStrikePulse& operator=(const RackStrikePulse&) = delete;

	/// Subscribed frames are repainted (whole subtree) when `poll` finds anything to show or to clear.
	FrameWatchers& watchers() { return watchers_; }

	/// LOOK AT THE MODEL AT `nowSeconds`; returns how many frames it told (0 when nothing drawn changed). The count is the evidence of a
	/// repaint, which cannot be observed headlessly.
	size_t poll(RackViewModel& model, double nowSeconds);
	~RackStrikePulse();
	/// Join the hub of the rack `key` names (the first call wins; an invalid key joins none). The surfaces that show the pulse do it when they are made.
	void join(const RackStrikeKey& key);
	/// The same at the pulse's own clock (`now`), which a case may replace.
	size_t poll(RackViewModel& model) { return poll(model, now()); }
	[[nodiscard]] double now() const { return clock_(); }
	void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

	/// HOW BRIGHT KEY `key` IS at `nowSeconds`, 0..1 (1 at the strike, linear to 0; steady 1 under reduced motion until the next look).
	[[nodiscard]] float level(int key, double nowSeconds) const noexcept;
	[[nodiscard]] bool live(double nowSeconds) const noexcept;

	void setReducedMotion(bool reduced) { reduced_ = reduced; }
	[[nodiscard]] bool reducedMotion() const noexcept { return reduced_; }

	/// WHETHER THIS PULSE IS ON A HUB (shares its rack's one look), TEST SEAM.
	[[nodiscard]] bool onHubForTest() const noexcept { return hub_ != nullptr; }
	/// How many frames the last look told (a repaint cannot be observed headlessly; this is its evidence), whoever's poll it was.
	[[nodiscard]] size_t lastToldForTest() const noexcept { return lastTold_; }

private:
	friend class RackStrikeHub;
	/// Take `mask` as this look: held (steady) or stamped, and the watchers told when anything is lit or just went out. `fresh` is a look that
	/// took (or was handed) a mask; a poll that arrives in a round another pulse already took leaves what that look gave.
	size_t look(const RackKeyMask& mask, double nowSeconds, bool fresh);
	/// FOLLOW PLAYED NOTE: the last struck key with a chain is viewed, and its page shown.
	static void follow(RackViewModel& model, const RackKeyMask& mask);
	RackStrikeKey key_ {};
	RackStrikeHub* hub_ = nullptr;
	FrameWatchers watchers_;
	std::function<double()> clock_;
	std::array<double, 128> struckAt_ {};
	RackKeyMask steady_ {};
	bool reduced_ = false;
	bool wasLive_ = false;
	size_t lastTold_ = 0;
};

} // namespace feathers::ui
