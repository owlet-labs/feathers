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

#include "fx/resonate/resonate_types.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::resonate {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT — its DECLARED default, never
/// zero (device_row.h's rule). A Mix read as zero would make a fresh device silent on its wet.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

/// A CONTINUOUS FIELD, CLAMPED TO ITS DECLARED RANGE. A non-finite value falls to the default.
[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	const float v = fieldOr(row, fieldsPresent, p);
	if (!std::isfinite(v)) {
		return static_cast<float>(d.def);
	}
	return std::clamp(v, static_cast<float>(d.min), static_cast<float>(d.max));
}

/// A STEPPED FIELD — a Choice or a count — ROUNDED AND CLAMPED INTO ITS LADDER.
[[nodiscard]] int steppedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	const float v = fieldOr(row, fieldsPresent, p);
	if (!std::isfinite(v)) {
		return static_cast<int>(d.def);
	}
	return std::clamp(static_cast<int>(std::lround(v)), static_cast<int>(d.min), static_cast<int>(d.max));
}

/// AN OFF/ON SWITCH. A non-finite value is its default, not Off — the same rule as every other field.
[[nodiscard]] bool switchOr(const Row& row, size_t fieldsPresent, Param p) {
	const float v = fieldOr(row, fieldsPresent, p);
	return std::isfinite(v) ? v >= 0.5f : kParamDescs[static_cast<size_t>(p)].def >= 0.5;
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = switchOr(row, fieldsPresent, Param::Enabled);
	s.model = steppedOr(row, fieldsPresent, Param::Model);
	s.character = rangedOr(row, fieldsPresent, Param::Character);
	s.morph = rangedOr(row, fieldsPresent, Param::Morph);
	s.bow = rangedOr(row, fieldsPresent, Param::Bow);
	s.drive = rangedOr(row, fieldsPresent, Param::Drive);
	s.pluck = rangedOr(row, fieldsPresent, Param::Pluck);
	s.voicing = steppedOr(row, fieldsPresent, Param::Voicing);
	s.porta = rangedOr(row, fieldsPresent, Param::Porta);
	s.octave = steppedOr(row, fieldsPresent, Param::Octave);
	s.semitone = steppedOr(row, fieldsPresent, Param::Semitone);
	s.fine = rangedOr(row, fieldsPresent, Param::Fine);
	s.droneOut = rangedOr(row, fieldsPresent, Param::DroneOut);
	s.droneExcite = rangedOr(row, fieldsPresent, Param::DroneExcite);
	s.droneAttack = rangedOr(row, fieldsPresent, Param::DroneAttack);
	s.droneDecay = rangedOr(row, fieldsPresent, Param::DroneDecay);
	s.droneChord = steppedOr(row, fieldsPresent, Param::DroneChord);
	s.droneCharacter = rangedOr(row, fieldsPresent, Param::DroneCharacter);
	s.droneWidth = rangedOr(row, fieldsPresent, Param::DroneWidth);
	s.root = steppedOr(row, fieldsPresent, Param::Root);
	s.size = rangedOr(row, fieldsPresent, Param::Size);
	s.width = rangedOr(row, fieldsPresent, Param::Width);
	s.blend = rangedOr(row, fieldsPresent, Param::Blend);
	s.level = rangedOr(row, fieldsPresent, Param::Level);
	s.mix = rangedOr(row, fieldsPresent, Param::Mix);
	s.sustain = rangedOr(row, fieldsPresent, Param::Sustain);
	s.jawari = rangedOr(row, fieldsPresent, Param::Jawari);
	s.bridgeCurve = rangedOr(row, fieldsPresent, Param::BridgeCurve);
	s.sympathy = rangedOr(row, fieldsPresent, Param::Sympathy);
	s.droneVoice = rangedOr(row, fieldsPresent, Param::DroneVoice);
	s.droneStiffness = rangedOr(row, fieldsPresent, Param::DroneStiffness);
	s.droneBody = rangedOr(row, fieldsPresent, Param::DroneBody);
	s.droneTwin = rangedOr(row, fieldsPresent, Param::DroneTwin);
	s.droneCycle = rangedOr(row, fieldsPresent, Param::DroneCycle);
	s.droneCycleSync = steppedOr(row, fieldsPresent, Param::DroneCycleSync);
	s.headRelease = steppedOr(row, fieldsPresent, Param::HeadRelease);
	s.droneTuning = steppedOr(row, fieldsPresent, Param::DroneTuning);
	s.droneCycleTransport = steppedOr(row, fieldsPresent, Param::DroneCycleTransport);
	s.pickup = rangedOr(row, fieldsPresent, Param::Pickup);
	s.bridgeFixed = rangedOr(row, fieldsPresent, Param::BridgeFixed);
	s.dronePluck = rangedOr(row, fieldsPresent, Param::DronePluck);
	s.droneJawari = rangedOr(row, fieldsPresent, Param::DroneJawari);
	s.droneBounce = rangedOr(row, fieldsPresent, Param::DroneBounce);
	s.droneSympathy = rangedOr(row, fieldsPresent, Param::DroneSympathy);
	s.droneToWetFx = steppedOr(row, fieldsPresent, Param::DroneToWetFx);
	s.droneThread = rangedOr(row, fieldsPresent, Param::DroneThread);
	s.droneFollow = steppedOr(row, fieldsPresent, Param::DroneFollow);
	s.droneDetail = steppedOr(row, fieldsPresent, Param::DroneDetail);
	s.ringDecay = rangedOr(row, fieldsPresent, Param::RingDecay);
	s.droneCycleLevel = rangedOr(row, fieldsPresent, Param::DroneCycleLevel);
	s.bounce = rangedOr(row, fieldsPresent, Param::Bounce);
	s.tension = rangedOr(row, fieldsPresent, Param::Tension);
	s.droneInput = rangedOr(row, fieldsPresent, Param::DroneInput);
	s.stringBlend = rangedOr(row, fieldsPresent, Param::StringBlend);
	s.stringExcite = rangedOr(row, fieldsPresent, Param::StringExcite);
	return s;
}

void packReading(const Reading& r, std::array<float, kReadingCount>& out) {
	out[kReadModel] = r.model;
	std::copy(r.nodes.begin(), r.nodes.end(), out.begin() + kReadNodes);
	std::copy(r.rest.begin(), r.rest.end(), out.begin() + kReadRest);
	std::copy(r.heads.begin(), r.heads.end(), out.begin() + kReadHeads);
	std::copy(r.headGains.begin(), r.headGains.end(), out.begin() + kReadHeadGains);
	out[kReadPluckPos] = r.pluckPos;
	out[kReadPluckAge] = r.pluckAge;
	out[kReadEnergy] = r.energy;
	out[kReadInLevel] = r.inLevel;
	out[kReadTravel] = r.travel;
	std::copy(r.headNotes.begin(), r.headNotes.end(), out.begin() + kReadHeadNotes);
	out[kReadParked] = r.parked;
	std::copy(r.droneHeads.begin(), r.droneHeads.end(), out.begin() + kReadDroneHeads);
	std::copy(r.droneGains.begin(), r.droneGains.end(), out.begin() + kReadDroneGains);
	std::copy(r.droneNotes.begin(), r.droneNotes.end(), out.begin() + kReadDroneNotes);
	out[kReadDroneCount] = r.droneCount;
}

Reading unpackReading(const float* values, size_t count) {
	Reading r;
	const auto at = [&](size_t i, float fallback) { return values != nullptr && i < count ? values[i] : fallback; };
	r.model = at(kReadModel, r.model);
	for (size_t i = 0; i < kPictureNodes; ++i) {
		r.nodes[i] = at(kReadNodes + i, 0.0f);
		r.rest[i] = at(kReadRest + i, 0.0f);
	}
	for (size_t i = 0; i < kMaxHeads; ++i) {
		r.heads[i] = at(kReadHeads + i, 0.0f);
		r.headGains[i] = at(kReadHeadGains + i, 0.0f);
	}
	r.pluckPos = at(kReadPluckPos, r.pluckPos);
	r.pluckAge = at(kReadPluckAge, r.pluckAge);
	r.energy = at(kReadEnergy, r.energy);
	r.inLevel = at(kReadInLevel, r.inLevel);
	r.travel = at(kReadTravel, r.travel);
	for (size_t i = 0; i < kMaxHeads; ++i) {
		r.headNotes[i] = at(kReadHeadNotes + i, 0.0f);
	}
	r.parked = at(kReadParked, r.parked);
	for (size_t i = 0; i < kMaxDroneStrings; ++i) {
		r.droneHeads[i] = at(kReadDroneHeads + i, 0.0f);
		r.droneGains[i] = at(kReadDroneGains + i, 0.0f);
		r.droneNotes[i] = at(kReadDroneNotes + i, 0.0f);
	}
	r.droneCount = at(kReadDroneCount, r.droneCount);
	return r;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Model)] = static_cast<float>(s.model);
	row[static_cast<size_t>(Param::Character)] = s.character;
	row[static_cast<size_t>(Param::Morph)] = s.morph;
	row[static_cast<size_t>(Param::Bow)] = s.bow;
	row[static_cast<size_t>(Param::Drive)] = s.drive;
	row[static_cast<size_t>(Param::Pluck)] = s.pluck;
	row[static_cast<size_t>(Param::Voicing)] = static_cast<float>(s.voicing);
	row[static_cast<size_t>(Param::Porta)] = s.porta;
	row[static_cast<size_t>(Param::Octave)] = static_cast<float>(s.octave);
	row[static_cast<size_t>(Param::Semitone)] = static_cast<float>(s.semitone);
	row[static_cast<size_t>(Param::Fine)] = s.fine;
	row[static_cast<size_t>(Param::DroneOut)] = s.droneOut;
	row[static_cast<size_t>(Param::DroneExcite)] = s.droneExcite;
	row[static_cast<size_t>(Param::DroneAttack)] = s.droneAttack;
	row[static_cast<size_t>(Param::DroneDecay)] = s.droneDecay;
	row[static_cast<size_t>(Param::DroneChord)] = static_cast<float>(s.droneChord);
	row[static_cast<size_t>(Param::DroneCharacter)] = s.droneCharacter;
	row[static_cast<size_t>(Param::DroneWidth)] = s.droneWidth;
	row[static_cast<size_t>(Param::Root)] = static_cast<float>(s.root);
	row[static_cast<size_t>(Param::Size)] = s.size;
	row[static_cast<size_t>(Param::Width)] = s.width;
	row[static_cast<size_t>(Param::Blend)] = s.blend;
	row[static_cast<size_t>(Param::Level)] = s.level;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::Sustain)] = s.sustain;
	row[static_cast<size_t>(Param::Jawari)] = s.jawari;
	row[static_cast<size_t>(Param::BridgeCurve)] = s.bridgeCurve;
	row[static_cast<size_t>(Param::Sympathy)] = s.sympathy;
	row[static_cast<size_t>(Param::DroneVoice)] = s.droneVoice;
	row[static_cast<size_t>(Param::DroneStiffness)] = s.droneStiffness;
	row[static_cast<size_t>(Param::DroneBody)] = s.droneBody;
	row[static_cast<size_t>(Param::DroneTwin)] = s.droneTwin;
	row[static_cast<size_t>(Param::DroneCycle)] = s.droneCycle;
	row[static_cast<size_t>(Param::DroneCycleSync)] = static_cast<float>(s.droneCycleSync);
	row[static_cast<size_t>(Param::HeadRelease)] = static_cast<float>(s.headRelease);
	row[static_cast<size_t>(Param::DroneTuning)] = static_cast<float>(s.droneTuning);
	row[static_cast<size_t>(Param::DroneCycleTransport)] = static_cast<float>(s.droneCycleTransport);
	row[static_cast<size_t>(Param::Pickup)] = s.pickup;
	row[static_cast<size_t>(Param::BridgeFixed)] = s.bridgeFixed;
	row[static_cast<size_t>(Param::DronePluck)] = s.dronePluck;
	row[static_cast<size_t>(Param::DroneJawari)] = s.droneJawari;
	row[static_cast<size_t>(Param::DroneBounce)] = s.droneBounce;
	row[static_cast<size_t>(Param::DroneSympathy)] = s.droneSympathy;
	row[static_cast<size_t>(Param::DroneToWetFx)] = static_cast<float>(s.droneToWetFx);
	row[static_cast<size_t>(Param::DroneThread)] = s.droneThread;
	row[static_cast<size_t>(Param::DroneFollow)] = static_cast<float>(s.droneFollow);
	row[static_cast<size_t>(Param::DroneDetail)] = static_cast<float>(s.droneDetail);
	row[static_cast<size_t>(Param::RingDecay)] = s.ringDecay;
	row[static_cast<size_t>(Param::DroneCycleLevel)] = s.droneCycleLevel;
	row[static_cast<size_t>(Param::Bounce)] = s.bounce;
	row[static_cast<size_t>(Param::Tension)] = s.tension;
	row[static_cast<size_t>(Param::DroneInput)] = s.droneInput;
	row[static_cast<size_t>(Param::StringBlend)] = s.stringBlend;
	row[static_cast<size_t>(Param::StringExcite)] = s.stringExcite;
	return row;
}

} // namespace feathers::fx::resonate
