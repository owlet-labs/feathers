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

#include "device/macro_type.h"
#include "device/registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <new>

// THE MACROS' STATE, FOR ANY COUNT — N values and N polarities, as `setFields` last handed them.
//
// **ONE IMPLEMENTATION BEHIND THE ONE-, FOUR- AND EIGHT-MACRO DEVICES.** The row's layout is a formula
// of N alone: macro n is field n, its polarity is field N + n, its type is field 2N + n and the enable
// is field 3N. For N = 4 that is the MACROS device's declared order exactly, so the four-macro device's
// descriptor and saved patches are not touched by this being a template.

namespace feathers::mod::macros {

[[nodiscard]] constexpr size_t macroField(size_t n) noexcept { return n; }
[[nodiscard]] constexpr size_t bipolarField(size_t count, size_t n) noexcept { return count + n; }
[[nodiscard]] constexpr size_t typeField(size_t count, size_t n) noexcept { return 2 * count + n; }
[[nodiscard]] constexpr size_t enableField(size_t count) noexcept { return 3 * count; }

template <size_t N>
class MacrosCore {
public:
	static constexpr size_t kCount = N;

	void prepare(double) noexcept {}
	/// THE RESOLVED ROW — each macro after the routes into it.
	void setFields(const float* fields, size_t count) noexcept {
		const auto at = [&](size_t i, float fallback) {
			return i < count && fields != nullptr ? fields[i] : fallback;
		};
		for (size_t n = 0; n < N; ++n) {
			// CLAMPED TO THE DECLARED 0..1, which the engine's resolve already keeps a routed value inside; a
			// row from anywhere else gets the same guarantee, so a bipolar output can never leave −1..1.
			// AND A NaN IS NOT A VALUE: `std::clamp` passes it, and it would reach every destination the macro drives.
			const float raw = at(macroField(n), 0.0f);
			value_[n] = std::isnan(raw) ? 0.0f : std::clamp(raw, 0.0f, 1.0f);
			bipolar_[n] = at(bipolarField(N, n), 0.0f) >= 0.5f;
		}
		valueR_ = value_; // MONO UNTIL `setFieldsR` SAYS OTHERWISE
	}
	/// NOTHING MOVES BETWEEN ROWS: a macro is where the row says, so a tick has nothing to advance.
	void tick() noexcept {}

	/// OUTPUT n: macro n's value, or `2v − 1` when its Bipolar is on. Zero past the last.
	[[nodiscard]] float outputAt(size_t n) const noexcept {
		if (n >= N) {
			return 0.0f;
		}
		// READ FROM THE ROW RATHER THAN FROM A TICK'S COPY, so what a pass publishes is the row that pass
		// resolved even when no tick fell inside it (a block shorter than `blockSamples`).
		return bipolar_[n] ? static_cast<float>(device::macroToBipolar(value_[n])) : value_[n];
	}
	/// THE RIGHT EAR OF EACH MACRO (backlog KU) — taken where `live` marks it, its left otherwise.
	void setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept {
		for (size_t n = 0; n < N; ++n) {
			const size_t i = macroField(n);
			if (fieldsR != nullptr && i < count && live.test(i)) {
				// as `setFields` clamps the left; a NaN leaves the right where it was (`std::clamp` would pass it)
				valueR_[n] = std::isnan(fieldsR[i]) ? valueR_[n] : std::clamp(fieldsR[i], 0.0f, 1.0f);
			}
		}
	}
	/// OUTPUT n IN BOTH EARS — each ear's value through the same polarity. `left` is `outputAt`.
	[[nodiscard]] device::EarPair pairAt(size_t n) const noexcept {
		if (n >= N) {
			return {};
		}
		const auto shaped = [&](float v) { return bipolar_[n] ? static_cast<float>(device::macroToBipolar(v)) : v; };
		return { shaped(value_[n]), shaped(valueR_[n]) };
	}

	/// **WHAT `setFields` DECODED**, for the registry-wide round-trip case (`test_device_ranges.cpp`), which
	/// has no other road back from a device that decodes into members. The types and the enable are
	/// absent because the device does not read them.
	[[nodiscard]] float valueForTest(size_t n) const noexcept { return n < N ? value_[n] : 0.0f; }
	[[nodiscard]] bool bipolarForTest(size_t n) const noexcept { return n < N && bipolar_[n]; }

private:
	std::array<float, N> value_ {};
	std::array<bool, N> bipolar_ {};
	/// The right ear (backlog KU): equal to `value_` after every `setFields`, until `setFieldsR` says otherwise.
	std::array<float, N> valueR_ {};
};

/// THE ONE SET OF MODULATOR OPS, per count. The ops of the four-macro device are the same code.
template <size_t N>
[[nodiscard]] constexpr device::ModOps makeModOps() noexcept {
	using Core = MacrosCore<N>;
	return device::ModOps {
		.stateBytes = sizeof(Core),
		.stateAlign = alignof(Core),
		.construct = [](void* storage) { new (storage) Core(); },
		.destruct = [](void* storage) { static_cast<Core*>(storage)->~Core(); },
		.prepare = [](void* storage, double sampleRate) { static_cast<Core*>(storage)->prepare(sampleRate); },
		.reset = [](void*) {},
		.setFields =
		    [](void* storage, const float* fields, size_t fieldsPresent) {
			    static_cast<Core*>(storage)->setFields(fields, fieldsPresent);
		    },
		// THE OTHER MODULATORS' RATE. Nothing here moves between rows, so the rate only sets how often the
		// host asks; `outputAt` reads the row either way.
		.blockSamples = 32,
		.tick = [](void* storage, const device::ModTickContext&) { static_cast<Core*>(storage)->tick(); },
		.outputAt = [](const void* storage, size_t output) { return static_cast<const Core*>(storage)->outputAt(output); },
		// NULL: a macro is a position, not a shape being run through, so there is nothing to mark.
		.playhead = nullptr,
		.outputPairAt = [](const void* storage, size_t output) { return static_cast<const Core*>(storage)->pairAt(output); },
		.setFieldsR =
		    [](void* storage, const float* fieldsR, size_t fieldsPresent, const device::ControlMask& live) {
			    static_cast<Core*>(storage)->setFieldsR(fieldsR, fieldsPresent, live);
		    },
	};
}

} // namespace feathers::mod::macros
