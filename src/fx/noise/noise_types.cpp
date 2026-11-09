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

#include "fx/noise/noise_types.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::noise {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT — its DECLARED default, never zero
/// (device_row.h's rule). A Level read as zero would silence a device that was asked for at its default.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	const float v = i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
	return std::isfinite(v) ? v : static_cast<float>(kParamDescs[i].def);
}

/// A CONTINUOUS FIELD, CLAMPED TO ITS DECLARED RANGE.
[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(fieldOr(row, fieldsPresent, p), static_cast<float>(d.min), static_cast<float>(d.max));
}

/// A CHOICE, ROUNDED AND CLAMPED INTO ITS LADDER.
[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, p))), static_cast<int>(d.min),
	                  static_cast<int>(d.max));
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.source = choiceOr(row, fieldsPresent, Param::Source);
	s.level = rangedOr(row, fieldsPresent, Param::Level);
	s.colour = rangedOr(row, fieldsPresent, Param::Colour);
	s.filterType = choiceOr(row, fieldsPresent, Param::FilterType);
	// CUTOFF MAY BE CARRIED PAST THE KNOB BY A ROUTE (`offsetsMayLeaveRange`), so it is clamped only to what the
	// filter can be asked for; the design clamps again under Nyquist.
	s.cutoff = std::clamp(fieldOr(row, fieldsPresent, Param::Cutoff), 0.0f, 16.0f);
	s.resonance = rangedOr(row, fieldsPresent, Param::Resonance);
	s.keyTrack = rangedOr(row, fieldsPresent, Param::KeyTrack);
	s.stereo = rangedOr(row, fieldsPresent, Param::Stereo);
	s.seedMode = choiceOr(row, fieldsPresent, Param::SeedMode);
	s.seed = choiceOr(row, fieldsPresent, Param::Seed);
	s.density = rangedOr(row, fieldsPresent, Param::Density);
	s.sync = choiceOr(row, fieldsPresent, Param::Sync);
	s.timing = choiceOr(row, fieldsPresent, Param::Timing);
	s.jitter = rangedOr(row, fieldsPresent, Param::Jitter);
	s.cluster = rangedOr(row, fieldsPresent, Param::Cluster);
	s.hitSize = choiceOr(row, fieldsPresent, Param::HitSize);
	s.randomPolarity = choiceOr(row, fieldsPresent, Param::Polarity) == 1;
	s.decay = rangedOr(row, fieldsPresent, Param::Decay);
	s.trig = rangedOr(row, fieldsPresent, Param::Trig);
	s.poly = fieldOr(row, fieldsPresent, Param::Poly) >= 0.5f;
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	const auto put = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	put(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	put(Param::Source, static_cast<float>(s.source));
	put(Param::Level, s.level);
	put(Param::Colour, s.colour);
	put(Param::FilterType, static_cast<float>(s.filterType));
	put(Param::Cutoff, s.cutoff);
	put(Param::Resonance, s.resonance);
	put(Param::KeyTrack, s.keyTrack);
	put(Param::Stereo, s.stereo);
	put(Param::SeedMode, static_cast<float>(s.seedMode));
	put(Param::Seed, static_cast<float>(s.seed));
	put(Param::Density, s.density);
	put(Param::Sync, static_cast<float>(s.sync));
	put(Param::Timing, static_cast<float>(s.timing));
	put(Param::Jitter, s.jitter);
	put(Param::Cluster, s.cluster);
	put(Param::HitSize, static_cast<float>(s.hitSize));
	put(Param::Polarity, s.randomPolarity ? 1.0f : 0.0f);
	put(Param::Decay, s.decay);
	put(Param::Trig, s.trig);
	put(Param::Poly, s.poly ? 1.0f : 0.0f);
	return row;
}

double hitsPerSecond(const Settings& s, double secondsPerBeat) {
	if (s.sync <= 0 || static_cast<size_t>(s.sync) > device::kDivisionCount) {
		return std::exp2(static_cast<double>(s.density));
	}
	const double spb = secondsPerBeat > 0.0 ? secondsPerBeat : kFallbackSecondsPerBeat;
	return 1.0 / (device::kDivisionBeats[static_cast<size_t>(s.sync - 1)] * spb);
}

dsp::RandomEventSettings eventSettingsOf(const Settings& s, double secondsPerBeat) {
	dsp::RandomEventSettings e;
	e.rateHz = hitsPerSecond(s, secondsPerBeat);
	e.timing = static_cast<dsp::EventTiming>(s.timing);
	e.jitter = s.jitter;
	e.cluster = s.cluster;
	e.size = static_cast<dsp::HitSize>(s.hitSize);
	e.randomPolarity = s.randomPolarity;
	return e;
}

} // namespace feathers::fx::noise
