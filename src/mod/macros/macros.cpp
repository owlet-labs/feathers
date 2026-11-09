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

#include "mod/macros/macros.h"

#include <algorithm>
#include <new>

namespace feathers::mod::macros {

void Macros::prepare(double) noexcept {}

void Macros::setFields(const float* fields, size_t count) noexcept {
	const auto at = [&](size_t i, float fallback) {
		return i < count && fields != nullptr ? fields[i] : fallback;
	};
	for (size_t n = 0; n < kMacroCount; ++n) {
		// CLAMPED TO THE DECLARED 0..1, which the engine's resolve already keeps a routed value inside; a
		// row from anywhere else gets the same guarantee, so a bipolar output can never leave −1..1.
		value_[n] = std::clamp(at(macroParam(n), 0.0f), 0.0f, 1.0f);
		bipolar_[n] = at(bipolarParam(n), 0.0f) >= 0.5f;
	}
	valueR_ = value_; // MONO UNTIL `setFieldsR` SAYS OTHERWISE
}

void Macros::setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept {
	for (size_t n = 0; n < kMacroCount; ++n) {
		const size_t i = macroParam(n);
		if (fieldsR != nullptr && i < count && live.test(i)) {
			valueR_[n] = std::clamp(fieldsR[i], 0.0f, 1.0f); // as `setFields` clamps the left
		}
	}
}

device::EarPair Macros::pairAt(size_t n) const noexcept {
	if (n >= kMacroCount) {
		return {};
	}
	const auto shaped = [&](float v) { return bipolar_[n] ? static_cast<float>(device::macroToBipolar(v)) : v; };
	return { shaped(value_[n]), shaped(valueR_[n]) };
}

float Macros::outputAt(size_t n) const noexcept {
	if (n >= kMacroCount) {
		return 0.0f;
	}
	// READ FROM THE ROW RATHER THAN FROM A TICK'S COPY, so what a pass publishes is the row that pass
	// resolved even when no tick fell inside it (a block shorter than `blockSamples`).
	return bipolar_[n] ? static_cast<float>(device::macroToBipolar(value_[n])) : value_[n];
}

} // namespace feathers::mod::macros

namespace feathers::mod::macros {

namespace {
Macros& asMacros(void* storage) noexcept { return *static_cast<Macros*>(storage); }
const Macros& asMacros(const void* storage) noexcept { return *static_cast<const Macros*>(storage); }
} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(Macros),
	.stateAlign = alignof(Macros),
	.construct = [](void* storage) { new (storage) Macros(); },
	.destruct = [](void* storage) { asMacros(storage).~Macros(); },
	.prepare = [](void* storage, double sampleRate) { asMacros(storage).prepare(sampleRate); },
	.reset = [](void*) {},
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asMacros(storage).setFields(fields, fieldsPresent);
	    },
	// THE OTHER MODULATORS' RATE. Nothing here moves between rows, so the rate only sets how often the
	// host asks; `outputAt` reads the row either way.
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asMacros(storage).tick(); },
	.outputAt = [](const void* storage, size_t output) { return asMacros(storage).outputAt(output); },
	// NULL: a macro is a position, not a shape being run through, so there is nothing to mark.
	.playhead = nullptr,
	.outputPairAt = [](const void* storage, size_t output) { return asMacros(storage).pairAt(output); },
	.setFieldsR =
	    [](void* storage, const float* fieldsR, size_t fieldsPresent, const device::ControlMask& live) {
		    asMacros(storage).setFieldsR(fieldsR, fieldsPresent, live);
	    },
};

} // namespace feathers::mod::macros
