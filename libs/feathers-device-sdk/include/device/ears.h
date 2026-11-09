/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// THE TWO EARS OF A MODULATION (backlog KU). A pair a source publishes, and a mask of the controls that hear one.
// Its own header because both `device.h`'s neighbours and the rack engine need it, and neither should pull the other.

namespace feathers::device {

/// WIDE ENOUGH FOR EVERY CONTROL A DEVICE MAY DECLARE — `registry.h` asserts `kMaxDeviceFields` fits.
/// FIVE WORDS since kMaxDeviceFields went 256 -> 320: a mask is whole words, so exactly 320 bits.
inline constexpr size_t kControlMaskWords = 5;
inline constexpr size_t kControlMaskBits = kControlMaskWords * 64;

/// **WHICH CONTROLS ARE STEREO-LIVE** — bit n is control n. `kControlMaskWords` words, no allocation, copied by value.
struct ControlMask {
	std::array<uint64_t, kControlMaskWords> words {};
	[[nodiscard]] constexpr bool test(size_t i) const noexcept {
		return i < kControlMaskBits && ((words[i >> 6] >> (i & 63)) & 1u) != 0;
	}
	constexpr void set(size_t i) noexcept {
		if (i < kControlMaskBits) {
			words[i >> 6] |= uint64_t { 1 } << (i & 63);
		}
	}
	[[nodiscard]] constexpr bool any() const noexcept {
		uint64_t all = 0;
		for (const uint64_t w : words) {
			all |= w;
		}
		return all != 0;
	}
	friend constexpr bool operator==(const ControlMask&, const ControlMask&) = default;
};

/// **ONE OUTPUT IN BOTH EARS.** `left` is what a mono destination reads (a mono destination
/// reads the LEFT ear), so it must equal the device's `outputAt` for the same output.
struct EarPair {
	float left = 0.0f;
	float right = 0.0f;
};

} // namespace feathers::device
