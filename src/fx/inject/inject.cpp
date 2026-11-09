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

#include "fx/inject/inject_types.h"

#include "fx/dyn/meter_ramp.h"

#include <algorithm>
#include <cmath>
#include <new>

// SIDECHAIN INJECT'S RENDER PATH — `out = (1 - m) * x + m * (g * side)`, per sample, with Mix, Gain and the enable ramped
// over `kRampSeconds` ("nothing steps"). See `inject_types.h`.

namespace feathers::fx::inject {

namespace {

[[nodiscard]] inline float dbToLin(float db) {
	return std::exp(db * 0.11512925464970229f);
}

[[nodiscard]] inline float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

struct OpsInject {
	Settings state {};
	/// THE SIDECHAIN FOR THE NEXT `process` (`AudioOps::setSidechain`): valid for that one call only; null is silence.
	const float* scL = nullptr;
	const float* scR = nullptr;
	int scFrames = 0;
	bool scSounding = false;

	dyn::LinearRamp mix, gain, bypass;
	dyn::HeldPeak injectedPeak;
	device::ReadingsSink sink {};

	void aimRamps() {
		mix.set(state.mix);
		gain.set(dbToLin(state.gainDb));
		bypass.set(state.enabled ? 1.0f : 0.0f);
	}

	void process(float* left, float* right, int frames) {
		aimRamps();
		for (int i = 0; i < frames; ++i) {
			const float xL = left[i];
			const float xR = right[i];
			// THE SIDECHAIN AT THIS SAMPLE: nothing connected (null, or a call shorter than the block) is silence.
			const bool have = scL != nullptr && scR != nullptr && i < scFrames;
			const float sL = have ? finiteOr0(scL[i]) : 0.0f;
			const float sR = have ? finiteOr0(scR[i]) : 0.0f;
			const float m = mix.next();
			const float g = gain.next();
			const float b = bypass.next();
			// DISABLED AND LANDED IS A PURE PASS-THROUGH (no sample touched, NaN included, as COMP's is).
			if (!state.enabled && bypass.settled() && b == 0.0f) {
				injectedPeak.push(0.0f);
				continue;
			}
			// **`(1-m)*x + m*wet` IS EXACT AT BOTH ENDS** — at m == 0 it is 1*x + 0*finite = x, at m == 1 it is 0*x + wet.
			const float wetL = g * sL;
			const float wetR = g * sR;
			const float yL = (1.0f - m) * xL + m * wetL;
			const float yR = (1.0f - m) * xR + m * wetR;
			left[i] = finiteOr0((1.0f - b) * xL + b * yL);
			right[i] = finiteOr0((1.0f - b) * xR + b * yR);
			injectedPeak.push(std::max(std::fabs(m * wetL), std::fabs(m * wetR)) * b);
		}
		// THE POINTERS WERE VALID FOR THIS CALL ONLY.
		scL = scR = nullptr;
		scFrames = 0;
		if (sink.publish != nullptr) {
			const float values[static_cast<size_t>(Reading::kCount)] = { injectedPeak.reading(), scSounding ? 1.0f : 0.0f };
			sink.publish(sink.context, values, static_cast<size_t>(Reading::kCount));
		}
	}
};

OpsInject& asInject(void* storage) { return *static_cast<OpsInject*>(storage); }

} // namespace

float gainLinForTest(const void* storage) { return static_cast<const OpsInject*>(storage)->gain.value(); }

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsInject),
    .stateAlign = alignof(OpsInject),
    .construct = [](void* storage) { new (storage) OpsInject(); },
    .destruct = [](void* storage) { asInject(storage).~OpsInject(); },
    .prepare =
        [](void* storage, double sampleRate, int) {
	        OpsInject& ops = asInject(storage);
	        for (dyn::LinearRamp* r : { &ops.mix, &ops.gain, &ops.bypass }) {
		        r->prepare(std::max(sampleRate, 1.0));
	        }
	        ops.injectedPeak.prepare(std::max(sampleRate, 1.0));
        },
    .reset =
        [](void* storage) {
	        OpsInject& ops = asInject(storage);
	        for (dyn::LinearRamp* r : { &ops.mix, &ops.gain, &ops.bypass }) {
		        r->reset();
	        }
	        ops.injectedPeak.reset();
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, so absent fields land on their DECLARED defaults.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asInject(storage).state = stateOf(row, take);
        },
    .processStereo = [](void* storage, float* left, float* right, int frames) { asInject(storage).process(left, right, frames); },
    // NO MONO ROAD: the sidechain is a stereo pair and the host hands a mono buffer as two.
    .processMono = nullptr,
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asInject(storage).sink = sink; },
    .setOuterSources = nullptr,
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    .windowedControls = {},
    .setFieldWindows = nullptr,
    .latencySamples = nullptr,
    // THE BUS ROAD HANDS THE PLUGIN'S SIDECHAIN just before `processStereo` (inside a Chain too).
    .setSidechain = [](void* storage, const float* left, const float* right, int frames, bool sounding) {
	    OpsInject& ops = asInject(storage);
	    ops.scL = left;
	    ops.scR = right;
	    ops.scFrames = frames;
	    ops.scSounding = sounding;
    },
};

} // namespace feathers::fx::inject
