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

#include "mod/math/math.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace feathers::mod::math {

namespace {

/// A STORED CHOICE, ROUNDED TO ITS STEP AND KEPT INSIDE ITS MENU. NaN is the first entry: `lround` of a
/// NaN is unspecified, so it never reaches it, and neither does a value too large for a `long`.
int choiceOf(float v, int count) noexcept {
	if (!(v >= 0.0f)) {
		return 0;
	}
	if (v >= static_cast<float>(count - 1)) {
		return count - 1;
	}
	return static_cast<int>(std::lround(v));
}

/// A FIELD THAT IS NOT A NUMBER READS AS THE CONTROL'S DEFAULT — `std::clamp` passes a NaN straight
/// through, and a NaN A would make every output NaN.
float finiteOr(float v, float fallback) noexcept { return std::isnan(v) ? fallback : v; }

} // namespace

void Math::prepare(double) noexcept {}

void Math::setFields(const float* fields, size_t count) noexcept {
	const auto at = [&](Param p, float fallback) {
		const auto i = static_cast<size_t>(p);
		return i < count && fields != nullptr ? finiteOr(fields[i], fallback) : fallback;
	};
	const auto def = [](Param p) { return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def); };
	// CLAMPED TO THE DECLARED RANGES, which the engine's resolve already keeps a routed value inside; a
	// row from anywhere else gets the same guarantee.
	a_ = std::clamp(at(Param::A, def(Param::A)), -1.0f, 1.0f);
	b_ = std::clamp(at(Param::B, def(Param::B)), -1.0f, 1.0f);
	op_ = static_cast<Op>(choiceOf(at(Param::Operator, def(Param::Operator)), static_cast<int>(Op::kCount)));
	map_ = static_cast<Map>(choiceOf(at(Param::Map, def(Param::Map)), static_cast<int>(Map::kCount)));
	amount_ = std::clamp(at(Param::Amount, def(Param::Amount)), 0.0f, 1.0f);
	bipolar_ = at(Param::Bipolar, def(Param::Bipolar)) >= 0.5f;
	octaves_ = std::clamp(at(Param::Periods, def(Param::Periods)), 0.0f, device::kPhiOctavesMax);
	exponent_ = std::clamp(at(Param::Exponent, def(Param::Exponent)), device::kExponentMin, device::kExponentMax);
	// MONO UNTIL `setFieldsR` SAYS OTHERWISE — the right ear is forgotten at every row.
	aR_ = a_;
	bR_ = b_;
	amountR_ = amount_;
}

void Math::setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept {
	// CLAMPED EXACTLY AS `setFields` CLAMPS THE LEFT, with the left as the fallback for a field the row lacks.
	const auto take = [&](Param p, float& field, float lo, float hi) {
		const auto i = static_cast<size_t>(p);
		if (fieldsR != nullptr && i < count && live.test(i)) {
			field = std::clamp(finiteOr(fieldsR[i], field), lo, hi);
		}
	};
	take(Param::A, aR_, -1.0f, 1.0f);
	take(Param::B, bR_, -1.0f, 1.0f);
	take(Param::Amount, amountR_, 0.0f, 1.0f);
}

float Math::outputAt(size_t n) const noexcept {
	if (n != 0) {
		return 0.0f;
	}
	// READ FROM THE ROW RATHER THAN FROM A TICK'S COPY, so what a pass publishes is the row that pass
	// resolved even when no tick fell inside it — MACROS' reason.
	return evaluate(a_, b_, op_, map_, amount_, bipolar_, octaves_, exponent_);
}

} // namespace feathers::mod::math

namespace feathers::mod::math {

namespace {
Math& asMath(void* storage) noexcept { return *static_cast<Math*>(storage); }
const Math& asMath(const void* storage) noexcept { return *static_cast<const Math*>(storage); }
} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(Math),
	.stateAlign = alignof(Math),
	.construct = [](void* storage) { new (storage) Math(); },
	.destruct = [](void* storage) { asMath(storage).~Math(); },
	.prepare = [](void* storage, double sampleRate) { asMath(storage).prepare(sampleRate); },
	.reset = [](void*) {},
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asMath(storage).setFields(fields, fieldsPresent);
	    },
	// THE OTHER MODULATORS' RATE. Nothing here moves between rows, so the rate only sets how often the
	// host asks; `outputAt` reads the row either way.
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asMath(storage).tick(); },
	.outputAt = [](const void* storage, size_t output) { return asMath(storage).outputAt(output); },
	// NULL: the output is a function of its inputs, not a shape being run through.
	.playhead = nullptr,
	.outputPairAt = [](const void* storage, size_t output) { return asMath(storage).pairAt(output); },
	.setFieldsR =
	    [](void* storage, const float* fieldsR, size_t fieldsPresent, const device::ControlMask& live) {
		    asMath(storage).setFieldsR(fieldsR, fieldsPresent, live);
	    },
};

} // namespace feathers::mod::math
