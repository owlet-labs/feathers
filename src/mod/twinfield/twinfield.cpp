/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "mod/twinfield/twinfield.h"

#include "fx/automod/automod_lfo.h"
#include "mod/modulator.h"

#include <cmath>
#include <new>

namespace feathers::mod::twinfield {

namespace amd = fx::automod;

void TwinField::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : 48000.0;
	reset();
}

void TwinField::reset() noexcept {
	for (Layer& layer : layers_) {
		layer.phase = 0.0;
		layer.readAt = 0.0;
		layer.onceRunning = false;
		layer.lock.release();
	}
}

void TwinField::setFields(const float* fields, size_t count) noexcept {
	settings_ = settingsOf(fields, count);
}

double TwinField::rateHzFor(const LayerSettings& l, const device::TransportClock& clock) noexcept {
	// AUTOMOD'S ROAD: Rate is a multiplier on whatever the Sync list selected; a division with no tempo
	// falls back to the first free rate, so a standalone build still moves (`Automod::effectiveRateHz`).
	const double multiplier = static_cast<double>(amd::rateMultiplierFor(l.rate));
	if (amd::isSyncedRate(l.sync) && clock.haveTempo()) {
		const double seconds = amd::syncDivisionBeats(l.sync) * clock.secondsPerBeat;
		if (seconds > 0.0) {
			return multiplier / seconds;
		}
	}
	const int choice = amd::isFreeRate(l.sync) ? l.sync : 0;
	return static_cast<double>(amd::freeRateHzFor(choice)) * multiplier;
}

/// AUTOMOD'S `songPeriod`, for a layer: a division reads BEATS, a free rate reads SECONDS, a stopped
/// transport is not a position (the accumulator runs), and nothing is invented where the host said nothing.
std::optional<TwinField::SongPeriod> TwinField::songPeriod(const LayerSettings& l,
                                                           const device::TransportClock& clock) const noexcept {
	if (!clock.playing) {
		return std::nullopt;
	}
	const double multiplier = static_cast<double>(amd::rateMultiplierFor(l.rate));
	if (!(multiplier > 0.0) || !std::isfinite(multiplier)) {
		return std::nullopt;
	}
	const double tickSeconds = static_cast<double>(kBlockSamples) / sampleRate_;
	if (amd::isSyncedRate(l.sync) && clock.haveTempo() && clock.havePositionBeats()) {
		const double periodBeats = amd::syncDivisionBeats(l.sync) / multiplier;
		if (periodBeats > 0.0 && std::isfinite(periodBeats)) {
			return SongPeriod { clock.songPositionBeats, periodBeats, tickSeconds / clock.secondsPerBeat,
				                kBeatsRoad };
		}
		return std::nullopt;
	}
	if (clock.havePositionSeconds()) {
		const int choice = amd::isFreeRate(l.sync) ? l.sync : 0;
		const double hz = static_cast<double>(amd::freeRateHzFor(choice)) * multiplier;
		if (hz > 0.0 && std::isfinite(hz)) {
			return SongPeriod { clock.songPositionSeconds, 1.0 / hz, tickSeconds, kSecondsRoad };
		}
	}
	return std::nullopt;
}

void TwinField::advance(Layer& layer, const LayerSettings& l, const device::ModTickContext& context) noexcept {
	const double step = rateHzFor(l, context.clock) * static_cast<double>(context.rateMul)
	                    * static_cast<double>(kBlockSamples) / sampleRate_;
	const double safeStep = std::isfinite(step) && step > 0.0 ? step : 0.0;
	switch (l.mode) {
	case Mode::Free: {
		// FREE IS POSITIONED BY THE SONG, BEFORE THE READ — AutoMod's backlog DQ, followed rather than
		// divided so a routed Rate does not scrub the orbit (`device::TransportLock`).
		const std::optional<SongPeriod> song = songPeriod(l, context.clock);
		if (song.has_value()) {
			const double turns = layer.lock.follow(song->position, song->period, song->tickLength, song->road);
			layer.phase = turns - std::floor(turns);
			layer.readAt = layer.phase;
			return;
		}
		layer.lock.release();
		layer.readAt = layer.phase;
		layer.phase += safeStep;
		layer.phase -= std::floor(layer.phase);
		return;
	}
	case Mode::Retrigger:
		layer.lock.release();
		if (context.retrigger) {
			layer.phase = 0.0;
		}
		layer.readAt = layer.phase;
		layer.phase += safeStep;
		layer.phase -= std::floor(layer.phase);
		return;
	case Mode::Once:
		layer.lock.release();
		if (context.retrigger) {
			layer.phase = 0.0;
			layer.onceRunning = true;
		}
		layer.readAt = layer.phase;
		if (layer.onceRunning) {
			layer.phase += safeStep;
			if (layer.phase >= 1.0) {
				// ONE ORBIT, THEN HOLD — at the start of the circle, which is where the orbit ends.
				layer.phase = 0.0;
				layer.onceRunning = false;
			}
		}
		return;
	case Mode::kCount:
		break;
	}
	layer.readAt = layer.phase;
}

void TwinField::tick(const device::ModTickContext& context) noexcept {
	for (size_t i = 0; i < kLayers; ++i) {
		Layer& layer = layers_[i];
		const LayerSettings& l = settings_.layers[i];
		advance(layer, l, context);
		// THE PLANE IS A FUNCTION OF (rotate, gamma): re-derived only when they move, keyed on the inputs
		// themselves (bitwise), AutoMod's derive gate.
		if (!(l.rotate == layer.planeRotate) || !(l.gamma == layer.planeGamma)) {
			layer.plane = planeFor(l.rotate, l.gamma);
			layer.planeRotate = l.rotate;
			layer.planeGamma = l.gamma;
		}
		layer.reading = readLayer(layer.plane, l, layer.readAt);
	}
	for (size_t k = 0; k < kPoints; ++k) {
		outputs_[k] = settings_.enabled ? blend(layers_[0].reading[k], layers_[1].reading[k], settings_.crossfade,
		                                        settings_.soloA, settings_.soloB)
		                                : 0.0f;
	}
}

} // namespace feathers::mod::twinfield

namespace feathers::mod::twinfield {

namespace {
TwinField& asField(void* storage) noexcept { return *static_cast<TwinField*>(storage); }
const TwinField& asField(const void* storage) noexcept { return *static_cast<const TwinField*>(storage); }
} // namespace

const device::ModOps kModOps {
	.stateBytes = sizeof(TwinField),
	.stateAlign = alignof(TwinField),
	.construct = [](void* storage) { new (storage) TwinField(); },
	.destruct = [](void* storage) { asField(storage).~TwinField(); },
	.prepare = [](void* storage, double sampleRate) { asField(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asField(storage).reset(); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asField(storage).setFields(fields, fieldsPresent);
	    },
	.blockSamples = static_cast<size_t>(kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext& context) { asField(storage).tick(context); },
	.outputAt = [](const void* storage, size_t output) { return asField(storage).outputAt(output); },
	// THE TWO ORBIT PHASES, ON THE PLAYHEAD CHANNEL EVERY HOST ALREADY CARRIES: A as the phase (ordinal 0), B as the level.
	// The card recomputes the eight live outputs from them and the modulated row through the same pure functions the
	// device publishes through (`readLayer`, `blend`), so its 2x4 block shows what a route receives.
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    *ordinal = 0;
		    *phase = static_cast<float>(asField(storage).phaseRead(0));
	    },
	.playheadLevel = [](const void* storage) { return static_cast<float>(asField(storage).phaseRead(1)); },
};

} // namespace feathers::mod::twinfield
