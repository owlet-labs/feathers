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

#include "mod/stereo/stereo.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace feathers::mod::stereo {

namespace {

float finiteOr(float v, float fallback) noexcept { return std::isnan(v) ? fallback : v; }

float defOf(Param p) noexcept { return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def); }

float fieldOr(const float* fields, size_t count, Param p) noexcept {
	const auto i = static_cast<size_t>(p);
	return i < count && fields != nullptr ? finiteOr(fields[i], defOf(p)) : defOf(p);
}

} // namespace

void StereoTool::prepare(double) noexcept {}

void StereoTool::setFields(const float* fields, size_t count) noexcept {
	amount_ = std::clamp(fieldOr(fields, count, Param::Amount), 0.0f, 2.0f);
	in_ = std::clamp(fieldOr(fields, count, Param::In), -1.0f, 1.0f);
	// MONO UNTIL `setFieldsR` SAYS OTHERWISE — the right ear is forgotten at every row.
	inR_ = in_;
	width_ = std::clamp(fieldOr(fields, count, Param::Width), 0.0f, 2.0f);
	swap_ = fieldOr(fields, count, Param::Swap) >= 0.5f;
}

void StereoTool::setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept {
	const auto in = static_cast<size_t>(Param::In);
	inR_ = live.test(in) && in < count && fieldsR != nullptr ? std::clamp(finiteOr(fieldsR[in], in_), -1.0f, 1.0f) : in_;
}

device::EarPair StereoTool::pairAt(size_t n) const noexcept {
	if (n >= static_cast<size_t>(Output::kCount)) {
		return {};
	}
	return evaluate(static_cast<Output>(n), in_, inR_, amount_, width_, swap_);
}

} // namespace feathers::mod::stereo

namespace feathers::mod::stereo {

namespace {
StereoTool& asTool(void* storage) noexcept { return *static_cast<StereoTool*>(storage); }
const StereoTool& asTool(const void* storage) noexcept { return *static_cast<const StereoTool*>(storage); }
} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(StereoTool),
	.stateAlign = alignof(StereoTool),
	.construct = [](void* storage) { new (storage) StereoTool(); },
	.destruct = [](void* storage) { asTool(storage).~StereoTool(); },
	.prepare = [](void* storage, double sampleRate) { asTool(storage).prepare(sampleRate); },
	.reset = [](void*) {},
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asTool(storage).setFields(fields, fieldsPresent);
	    },
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asTool(storage).tick(); },
	.outputAt = [](const void* storage, size_t output) { return asTool(storage).outputAt(output); },
	.playhead = nullptr,
	.outputPairAt = [](const void* storage, size_t output) { return asTool(storage).pairAt(output); },
	.setFieldsR =
	    [](void* storage, const float* fieldsR, size_t fieldsPresent, const device::ControlMask& live) {
		    asTool(storage).setFieldsR(fieldsR, fieldsPresent, live);
	    },
};

} // namespace feathers::mod::stereo
