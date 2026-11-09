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

#include "fx/spectral/spectral_types.h"

#include "dsp/vox_anchors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::fx::spectral {

namespace {

[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(fieldOr(row, fieldsPresent, p), static_cast<float>(d.min), static_cast<float>(d.max));
}

[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, p))), static_cast<int>(d.min),
	                  static_cast<int>(d.max));
}

[[nodiscard]] bool switchOr(const Row& row, size_t fieldsPresent, Param p) { return fieldOr(row, fieldsPresent, p) >= 0.5f; }

} // namespace

Settings stateOf(const Row& row, size_t n) {
	Settings s;
	s.enabled = switchOr(row, n, Param::Enabled);
	s.resolution = static_cast<Resolution>(choiceOr(row, n, Param::Resolution));
	s.mix = rangedOr(row, n, Param::Mix);
	s.keyTrack = switchOr(row, n, Param::KeyTrack);
	s.keyOffset = rangedOr(row, n, Param::KeyOffset);
	s.poly = switchOr(row, n, Param::Poly);
	s.filterOn = switchOr(row, n, Param::FilterOn);
	s.filterMode = choiceOr(row, n, Param::FilterMode);
	s.filterCutoff = rangedOr(row, n, Param::FilterCutoff);
	s.filterResonance = rangedOr(row, n, Param::FilterResonance);
	s.filterSlope = rangedOr(row, n, Param::FilterSlope);
	s.filterGain = rangedOr(row, n, Param::FilterGain);
	s.filter2On = switchOr(row, n, Param::Filter2On);
	s.filter2Mode = choiceOr(row, n, Param::Filter2Mode);
	s.filter2Cutoff = rangedOr(row, n, Param::Filter2Cutoff);
	s.filter2Resonance = rangedOr(row, n, Param::Filter2Resonance);
	s.filter2Slope = rangedOr(row, n, Param::Filter2Slope);
	s.filter2Gain = rangedOr(row, n, Param::Filter2Gain);
	s.filterParallel = switchOr(row, n, Param::FilterRouting);
	s.combOn = switchOr(row, n, Param::CombOn);
	s.combSpacing = rangedOr(row, n, Param::CombSpacing);
	s.combDepth = rangedOr(row, n, Param::CombDepth);
	s.combPhase = rangedOr(row, n, Param::CombPhase);
	s.vowelOn = switchOr(row, n, Param::VowelOn);
	s.vowel = rangedOr(row, n, Param::Vowel);
	s.vowelDepth = rangedOr(row, n, Param::VowelDepth);
	s.tiltOn = switchOr(row, n, Param::TiltOn);
	s.tiltAmount = rangedOr(row, n, Param::TiltAmount);
	s.oddEvenOn = switchOr(row, n, Param::OddEvenOn);
	s.oddEvenBalance = rangedOr(row, n, Param::OddEvenBalance);
	s.vocodeOn = switchOr(row, n, Param::VocodeOn);
	s.vocodeBands = rangedOr(row, n, Param::VocodeBands);
	s.vocodeAttackMs = rangedOr(row, n, Param::VocodeAttack);
	s.vocodeReleaseMs = rangedOr(row, n, Param::VocodeRelease);
	s.vocodeFlatten = rangedOr(row, n, Param::VocodeFlatten);
	s.vocodeSibilance = rangedOr(row, n, Param::VocodeSibilance);
	s.vocodeCarrier = choiceOr(row, n, Param::VocodeCarrier);
	s.vocodeDepth = rangedOr(row, n, Param::VocodeDepth);
	s.vocodeGateDb = rangedOr(row, n, Param::VocodeGate);
	s.vocodeFreeze = switchOr(row, n, Param::VocodeFreeze);
	s.vocodeLow = rangedOr(row, n, Param::VocodeLow);
	s.vocodeHigh = rangedOr(row, n, Param::VocodeHigh);
	s.vocodeStereo = choiceOr(row, n, Param::VocodeStereo);
	s.vocodeBandwidth = rangedOr(row, n, Param::VocodeBandwidth);
	s.vocodeZone = choiceOr(row, n, Param::VocodeZone);
	s.vocodeZoneAmount = rangedOr(row, n, Param::VocodeZoneAmount);
	s.vocodeZoneFree = rangedOr(row, n, Param::VocodeZoneFree);
	s.vocodeWidthZone = choiceOr(row, n, Param::VocodeWidthZone);
	s.vocodeWidthAmount = rangedOr(row, n, Param::VocodeWidthAmount);
	s.vocodeWidthFree = rangedOr(row, n, Param::VocodeWidthFree);
	s.fshiftOn = switchOr(row, n, Param::FShiftOn);
	s.fshiftLow = rangedOr(row, n, Param::FShiftLow);
	s.fshiftHigh = rangedOr(row, n, Param::FShiftHigh);
	s.fshiftCrossover = rangedOr(row, n, Param::FShiftCrossover);
	s.fshiftLink = switchOr(row, n, Param::FShiftLink);
	s.partialsOn = switchOr(row, n, Param::PartialsOn);
	s.partialsMode = choiceOr(row, n, Param::PartialsMode);
	s.partialsCount = rangedOr(row, n, Param::PartialsCount);
	s.partialsSet = rangedOr(row, n, Param::PartialsSet);
	s.partialsDepth = rangedOr(row, n, Param::PartialsDepth);
	s.curveOn = switchOr(row, n, Param::CurveOn);
	// THE CURVE IS NOT IN THE ROW: it is the hand's block (`spectral_curve_db`), which the device takes by `pullHandBlocks`
	// and the editor reads from the host, so a row decodes to a FLAT curve here and whoever has the block fills it in.
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	const auto put = [&](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	const auto flag = [](bool b) { return b ? 1.0f : 0.0f; };
	put(Param::Enabled, flag(s.enabled));
	put(Param::Resolution, static_cast<float>(static_cast<int>(s.resolution)));
	put(Param::Mix, s.mix);
	put(Param::KeyTrack, flag(s.keyTrack));
	put(Param::KeyOffset, s.keyOffset);
	put(Param::Poly, flag(s.poly));
	put(Param::FilterOn, flag(s.filterOn));
	put(Param::FilterMode, static_cast<float>(s.filterMode));
	put(Param::FilterCutoff, s.filterCutoff);
	put(Param::FilterResonance, s.filterResonance);
	put(Param::FilterSlope, s.filterSlope);
	put(Param::FilterGain, s.filterGain);
	put(Param::Filter2On, flag(s.filter2On));
	put(Param::Filter2Mode, static_cast<float>(s.filter2Mode));
	put(Param::Filter2Cutoff, s.filter2Cutoff);
	put(Param::Filter2Resonance, s.filter2Resonance);
	put(Param::Filter2Slope, s.filter2Slope);
	put(Param::Filter2Gain, s.filter2Gain);
	put(Param::FilterRouting, flag(s.filterParallel));
	put(Param::CombOn, flag(s.combOn));
	put(Param::CombSpacing, s.combSpacing);
	put(Param::CombDepth, s.combDepth);
	put(Param::CombPhase, s.combPhase);
	put(Param::VowelOn, flag(s.vowelOn));
	put(Param::Vowel, s.vowel);
	put(Param::VowelDepth, s.vowelDepth);
	put(Param::TiltOn, flag(s.tiltOn));
	put(Param::TiltAmount, s.tiltAmount);
	put(Param::OddEvenOn, flag(s.oddEvenOn));
	put(Param::OddEvenBalance, s.oddEvenBalance);
	put(Param::VocodeOn, flag(s.vocodeOn));
	put(Param::VocodeBands, s.vocodeBands);
	put(Param::VocodeAttack, s.vocodeAttackMs);
	put(Param::VocodeRelease, s.vocodeReleaseMs);
	put(Param::VocodeFlatten, s.vocodeFlatten);
	put(Param::VocodeSibilance, s.vocodeSibilance);
	put(Param::VocodeCarrier, static_cast<float>(s.vocodeCarrier));
	put(Param::VocodeDepth, s.vocodeDepth);
	put(Param::VocodeGate, s.vocodeGateDb);
	put(Param::VocodeFreeze, flag(s.vocodeFreeze));
	put(Param::VocodeLow, s.vocodeLow);
	put(Param::VocodeHigh, s.vocodeHigh);
	put(Param::VocodeStereo, static_cast<float>(s.vocodeStereo));
	put(Param::VocodeBandwidth, s.vocodeBandwidth);
	put(Param::VocodeZone, static_cast<float>(s.vocodeZone));
	put(Param::VocodeZoneAmount, s.vocodeZoneAmount);
	put(Param::VocodeZoneFree, s.vocodeZoneFree);
	put(Param::VocodeWidthZone, static_cast<float>(s.vocodeWidthZone));
	put(Param::VocodeWidthAmount, s.vocodeWidthAmount);
	put(Param::VocodeWidthFree, s.vocodeWidthFree);
	put(Param::FShiftOn, flag(s.fshiftOn));
	put(Param::FShiftLow, s.fshiftLow);
	put(Param::FShiftHigh, s.fshiftHigh);
	put(Param::FShiftCrossover, s.fshiftCrossover);
	put(Param::FShiftLink, flag(s.fshiftLink));
	put(Param::PartialsOn, flag(s.partialsOn));
	put(Param::PartialsMode, static_cast<float>(s.partialsMode));
	put(Param::PartialsCount, s.partialsCount);
	put(Param::PartialsSet, s.partialsSet);
	put(Param::PartialsDepth, s.partialsDepth);
	put(Param::CurveOn, flag(s.curveOn));
	return row;
}

shaper::ShaperParams shaperParamsOf(const Settings& s, double f0) {
	shaper::ShaperParams p;
	p.enabled = s.enabled;
	p.filter.on = s.filterOn;
	p.filter.mode = static_cast<shaper::FilterMode>(std::clamp(s.filterMode, 0, shaper::kFilterModeCount - 1));
	p.filter.cutoffHz = static_cast<float>(std::exp2(static_cast<double>(s.filterCutoff)));
	p.filter.resonance = s.filterResonance;
	p.filter.slope = s.filterSlope;
	p.filter.gainDb = s.filterGain;
	// KEY TRACK AT 1 (the audition, 2026-10-13: every operator follows f0): the cutoff is held as a harmonic of f0, pivoting at
	// C3 (shaper::kKeyTrackPivotHz is kFixedF0Hz), so with Key Track off and Offset 0 it is exactly where its knob says.
	p.filter.keyTrack = 1.0f;
	// THE SECOND FILTER AND THE ROUTING (F2): the first's road exactly, Key track at 1 too; a device has one set, so each filter's
	// presence is whole (ShaperParams' default).
	p.filter2.on = s.filter2On;
	p.filter2.mode = static_cast<shaper::FilterMode>(std::clamp(s.filter2Mode, 0, shaper::kFilterModeCount - 1));
	p.filter2.cutoffHz = static_cast<float>(std::exp2(static_cast<double>(s.filter2Cutoff)));
	p.filter2.resonance = s.filter2Resonance;
	p.filter2.slope = s.filter2Slope;
	p.filter2.gainDb = s.filter2Gain;
	p.filter2.keyTrack = 1.0f;
	p.parallel = s.filterParallel ? 1.0f : 0.0f;
	// FULLY OPEN AT THE END OF ITS TRAVEL (Z7c/Z7d): the shared shaper's rule, `shaper::filterOpenness` — the control at its top
	// (Low Pass) or bottom (High Pass), or the corner at the plot's edge, and the filter is off; it fades out over the last third
	// of an octave before.
	p.comb.on = s.combOn;
	p.comb.spacing = s.combSpacing;
	p.comb.depth = s.combDepth;
	p.comb.phase = s.combPhase;
	p.vowel.on = s.vowelOn;
	const VowelFormants v = vowelFormants(static_cast<double>(s.vowel));
	// THE VOWEL MOVES WITH f0's DISTANCE FROM C3: its formants are the anchors' at C3, scaled by f0 / C3.
	const double vowelScale = f0 / kFixedF0Hz;
	p.vowel.f1Hz = static_cast<float>(v.f1Hz * vowelScale);
	p.vowel.f2Hz = static_cast<float>(v.f2Hz * vowelScale);
	p.vowel.depth = s.vowelDepth;
	p.tilt.on = s.tiltOn;
	p.tilt.dbPerOctave = s.tiltAmount;
	p.tilt.pivotHz = kTiltPivotHz;
	p.oddEven.on = s.oddEvenOn;
	p.oddEven.balance = s.oddEvenBalance;
	// THE CURVE IS IN HARMONICS OF f0, as the drawer's is.
	p.curve.on = s.curveOn;
	p.curve.gain = curveGainsOf(s.curveDb);
	return p;
}

double shaperF0(const Settings& s, bool tracked, double noteHz) {
	return (tracked ? noteHz : kFixedF0Hz) * std::exp2(static_cast<double>(s.keyOffset) / 12.0);
}

bool binGains(const Settings& s, bool tracked, double noteHz, double sampleRate, int fftSize, BinScratch scratch,
              float* out) noexcept {
	const int bins = fftSize / 2 + 1;
	const double f0 = shaperF0(s, tracked, noteHz);
	const shaper::PreparedShaper prepared = shaper::prepare(shaperParamsOf(s, f0), f0);
	if (prepared.identity) {
		std::fill_n(out, bins, 1.0f);
		return true;
	}
	const double binHz = sampleRate / static_cast<double>(fftSize);
	for (int i = 0; i < bins; ++i) {
		const double hz = std::max(static_cast<double>(i) * binHz, shaper::kMinHz);
		scratch.hz[i] = static_cast<float>(hz);
		scratch.k[i] = static_cast<float>(hz / f0);
		// PINK AND STATIC: Tilt and the vowel are level-compensated per octave, and the curve never reads the signal.
		scratch.base[i] = static_cast<float>(1.0 / std::sqrt(std::max(hz, 20.0)));
	}
	shaper::gains(prepared, scratch.k, scratch.hz, scratch.base, bins, out);
	return false;
}

} // namespace feathers::fx::spectral

namespace feathers::fx::spectral {

void noteName(int midi, char* out, size_t size) {
	static constexpr const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	const int pc = ((midi % 12) + 12) % 12;
	const int octave = (midi - pc) / 12 - 2; // 60 is C3
	std::snprintf(out, size, "%s%d", kNames[pc], octave);
}

namespace {
bool formatControl(size_t control, double value, const device::TextView& view, char* out, size_t size) {
	// THE GATE AT ITS BOTTOM IS OFF (it gates nothing there), so it says so rather than "-96.0dB".
	if (control == static_cast<size_t>(Param::VocodeGate) && value <= kVocodeGateOffDb + 1e-6) {
		std::snprintf(out, size, "Off");
		return true;
	}
	if (control != static_cast<size_t>(Param::KeyOffset)) {
		return false;
	}
	const bool keyTrack = view.field != nullptr && view.field(view.host, static_cast<size_t>(Param::KeyTrack)) >= 0.5f;
	const bool whole = std::fabs(value - std::round(value)) < 0.005;
	if (keyTrack) {
		std::snprintf(out, size, whole ? "%+.0f st" : "%+.2f st", value);
		return true;
	}
	char name[16];
	noteName(kFixedF0Midi + static_cast<int>(std::lround(value)), name, sizeof(name));
	std::snprintf(out, size, whole ? "%s %+.0f" : "%s %+.2f", name, value);
	return true;
}
} // namespace

const device::TextOps kTextOps { .format = &formatControl };

} // namespace feathers::fx::spectral
