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

#pragma once

#include "device/transport_lock.h"
#include "mod/twinfield/twinfield_types.h"

#include <array>
#include <limits>
#include <optional>

namespace feathers::mod::twinfield {

/// THE DEVICE. Holds no randomness and captures nothing: its outputs are a pure function of the row and
/// each layer's orbit phase, and the phase is derived from the song (Free, playing) or counted from reset
/// or the last note. So there is no seed and no extra state to persist — the keyed params are the patch.
class TwinField {
public:
	void prepare(double sampleRate) noexcept;
	void reset() noexcept;
	void setFields(const float* fields, size_t count) noexcept;
	void tick(const device::ModTickContext& context) noexcept;
	[[nodiscard]] float outputAt(size_t n) const noexcept { return n < kPoints ? outputs_[n] : 0.0f; }

	[[nodiscard]] const Settings& settingsForTest() const noexcept { return settings_; }
	/// THE ORBIT PHASE, in turns, that the most recent tick READ at — what the playhead channel publishes.
	[[nodiscard]] double phaseRead(size_t layer) const noexcept { return layers_[layer].readAt; }
	/// THE ORBIT PHASE, in turns, that the most recent tick READ at.
	[[nodiscard]] double phaseForTest(size_t layer) const noexcept { return layers_[layer].readAt; }
	[[nodiscard]] float readingForTest(size_t layer, size_t k) const noexcept { return layers_[layer].reading[k]; }

	/// WHAT ONE LAYER'S ORBIT RUNS AT, in Hz — AutoMod's `effectiveRateHz`, restated for a layer.
	[[nodiscard]] static double rateHzFor(const LayerSettings& l, const device::TransportClock& clock) noexcept;

private:
	struct SongPeriod {
		double position;
		double period;
		double tickLength;
		int road;
	};
	static constexpr int kBeatsRoad = 1;
	static constexpr int kSecondsRoad = 2;

	struct Layer {
		double phase = 0.0;  ///< the accumulator, turns
		double readAt = 0.0; ///< the phase the last tick read, turns
		bool onceRunning = false;
		device::TransportLock lock;
		Plane plane {};
		double planeRotate = std::numeric_limits<double>::quiet_NaN();
		double planeGamma = std::numeric_limits<double>::quiet_NaN();
		std::array<float, kPoints> reading {};
	};

	[[nodiscard]] std::optional<SongPeriod> songPeriod(const LayerSettings& l,
	                                                   const device::TransportClock& clock) const noexcept;
	void advance(Layer& layer, const LayerSettings& l, const device::ModTickContext& context) noexcept;

	double sampleRate_ = 48000.0;
	Settings settings_ {};
	std::array<Layer, kLayers> layers_ {};
	std::array<float, kPoints> outputs_ {};
};

} // namespace feathers::mod::twinfield
