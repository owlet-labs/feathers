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

#include "fx/spectral/spectral.h"
#include "fx/scrub.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace feathers::fx::spectral {

void SpectralDevice::prepare(double sampleRate) {
	stft_.prepare(sampleRate);
	int bins = 0;
	for (int r = 0; r < kResolutionCount; ++r) {
		bins = std::max(bins, fftSizeFor(static_cast<Resolution>(r), sampleRate) / 2 + 1);
	}
	k_.assign(static_cast<size_t>(bins), 0.0f);
	hz_.assign(static_cast<size_t>(bins), 0.0f);
	base_.assign(static_cast<size_t>(bins), 0.0f);
	stft_.setResolution(target_.resolution);
	rearmFreeze(); // Z10c: a re-activation (a restart) re-prepares; a held envelope comes back
	dirty_ = true;
}

void SpectralDevice::reset() noexcept {
	stft_.reset();
	frozenTracked_ = false; // a reset un-primes the frozen envelope: the next freeze remembers its own note
	rearmFreeze();          // Z10c: unless Freeze is on, which re-primes it from the last published block
	dirty_ = true;
}

void SpectralDevice::setTarget(const Settings& given) noexcept {
	// THE DRAWN CURVE IS NOT IN THE ROW (backlog: row headroom): it is the hand's block (`pullHandBlocks`), so whatever the
	// row decoded for it is replaced by the instance's own copy before anything compares or uses it.
	Settings s = given;
	s.curveDb = curveCopy_.values;
	if (s == target_) {
		return;
	}
	target_ = s;
	const bool restop = s.resolution != stft_.resolution();
	stft_.setResolution(s.resolution); // a no-op unless the stop moved
	if (restop) {
		rearmFreeze(); // Z10c: a new stop resets the STFT; a held envelope comes back at it
	}
	stft_.setMix(s.mix);
	Stft::Vocode v;
	v.on = s.vocodeOn && s.enabled;
	v.bands = s.vocodeBands;
	v.attackMs = s.vocodeAttackMs;
	v.releaseMs = s.vocodeReleaseMs;
	v.flatten = s.vocodeFlatten;
	v.sibilance = s.vocodeSibilance;
	v.carrier = std::clamp(s.vocodeCarrier, 0, static_cast<int>(std::size(kCarrierNames)) - 1);
	v.depth = s.vocodeDepth;
	v.gateDb = s.vocodeGateDb;
	v.freeze = s.vocodeFreeze;
	v.lowHz = std::exp2(static_cast<double>(s.vocodeLow));
	v.highHz = std::exp2(static_cast<double>(s.vocodeHigh));
	v.stereo = std::clamp(s.vocodeStereo, 0, 2);
	v.bandwidth = s.vocodeBandwidth;
	v.zone = std::clamp(s.vocodeZone, 0, static_cast<int>(std::size(kZoneNames)) - 1);
	v.zoneAmount = s.vocodeZoneAmount;
	v.zoneFree = s.vocodeZoneFree;
	v.widthZone = std::clamp(s.vocodeWidthZone, 0, static_cast<int>(std::size(kWidthZoneNames)) - 1);
	v.widthAmount = s.vocodeWidthAmount;
	v.widthFree = s.vocodeWidthFree;
	stft_.setVocode(v);
	Stft::Partials partials;
	partials.on = s.partialsOn && s.enabled;
	partials.mode = std::clamp(s.partialsMode, 0, 2);
	partials.count = s.partialsCount;
	partials.depth = s.partialsDepth;
	partials.centersSet = s.partialsSet;
	stft_.setPartials(partials);
	dirty_ = true;
}

void SpectralDevice::pullCurve(const SpectralDevice& source) noexcept {
	if (curveCopy_.pull(source.handCurve_, curveScratch_)) {
		for (float& v : curveCopy_.values) {
			v = std::isfinite(v) ? std::clamp(v, kCurveFloorDb, kCurveMaxDb) : 0.0f;
		}
		target_.curveDb = curveCopy_.values;
		dirty_ = true;
	}
}

bool SpectralDevice::applyCurve(const float* in, size_t n) noexcept {
	if (n != static_cast<size_t>(shaper::kCurveBands)) {
		return false;
	}
	std::array<float, shaper::kCurveBands> clean {};
	for (size_t b = 0; b < clean.size(); ++b) {
		clean[b] = std::isfinite(in[b]) ? std::clamp(in[b], kCurveFloorDb, kCurveMaxDb) : 0.0f;
	}
	handCurve_.write(clean.data(), clean.size());
	return true;
}

bool SpectralDevice::restoreCurve(const float* in, size_t n) noexcept {
	if (!applyCurve(in, n)) {
		return false;
	}
	pullCurve(*this); // nothing runs this instance yet: take it now, so a prepare or reset before the first block keeps it
	return true;
}

void SpectralDevice::setNoteContext(const device::NoteContext& note) noexcept {
	noteSeen_ = true;
	noteSemis_ = static_cast<double>(note.pitchSemitones);
	const double hz = 440.0 * std::exp2((static_cast<double>(note.pitchSemitones) - 69.0) / 12.0);
	if (hz != noteHz_) {
		noteHz_ = hz;
		// Latched always, so switching Key Track on picks it up; the curve is recomputed only while it is read.
		dirty_ = dirty_ || target_.keyTrack;
	}
}

void SpectralDevice::trackFreeze() noexcept {
	// **FREEZE RESPECTS KEY TRACK (Z10, Requirement: no switch).** With Key Track on, the note as Freeze takes hold (the last note, or C3)
	// is remembered, and while frozen the held envelope moves by the note's distance from it — the NOTE only, never Offset, as the
	// Square follows it (Z9). A glide moves it continuously. With Key Track off it stays where it was caught, in Hz.
	const bool tracking = target_.vocodeOn && target_.vocodeFreeze && target_.keyTrack;
	if (tracking && !frozenTracked_) {
		frozenNoteSemis_ = noteSemis_;
	}
	frozenTracked_ = tracking;
	stft_.setFreezeShift(tracking ? noteSemis_ - frozenNoteSemis_ : 0.0);
}

// **THE FROZEN ENVELOPE IN THE PATCH (Z10b).** The words, in order: per sidechain channel, whether it holds, its grid in dBFS
// (each float by its bits) and its flatness; then whether Key Track's note was remembered, and that note (a double).
void SpectralDevice::publishFreeze() noexcept {
	const uint32_t mask = (stft_.frozen(0) ? 1u : 0u) | (stft_.frozen(1) ? 2u : 0u);
	const double note = frozenTracked_ ? frozenNoteSemis_ : 0.0;
	if (mask == publishedFreezeMask_ && frozenTracked_ == publishedFreezeTracked_ && note == publishedFreezeNote_) {
		return; // a held envelope does not move, so neither does its block
	}
	publishedFreezeMask_ = mask;
	publishedFreezeTracked_ = frozenTracked_;
	publishedFreezeNote_ = note;
	std::array<float, kFreezeWords> words {};
	device::BlockWriter w(words.data(), words.size());
	const double toFs = stft_.envelopeToFs();
	for (int k = 0; k < 2; ++k) {
		const bool held = (mask & (1u << k)) != 0;
		w.flag(held);
		const auto& grid = stft_.sidechainEnvelope(k);
		for (const float db : grid) {
			w.f32(held ? static_cast<float>(static_cast<double>(db) + toFs) : 0.0f);
		}
		w.f32(held ? stft_.sidechainFlatness(k) : 0.0f);
	}
	w.flag(frozenTracked_);
	w.f64(note);
	freezeSnapshot_.publish(words.data(), w.size());
}

bool SpectralDevice::restoreFreeze(const float* in, size_t n) noexcept {
	if (n != kFreezeWords) {
		return false;
	}
	PendingFreeze p;
	device::BlockReader r(in, n);
	for (size_t k = 0; k < 2; ++k) {
		p.held[k] = r.flag();
		for (float& db : p.dbfs[k]) {
			db = r.f32();
		}
		p.flatness[k] = r.f32();
	}
	p.tracked = r.flag();
	p.noteSemis = r.f64();
	if (!r.ok()) {
		return false;
	}
	// A HELD CHANNEL MUST BE A NUMBER THROUGHOUT: a damaged file plays as freshly built rather than holding a NaN.
	for (size_t k = 0; k < 2; ++k) {
		bool finite = std::isfinite(p.flatness[k]);
		for (const float db : p.dbfs[k]) {
			finite = finite && std::isfinite(db);
		}
		if (p.held[k] && !finite) {
			return false;
		}
	}
	if (p.tracked && !std::isfinite(p.noteSemis)) {
		return false;
	}
	p.armed = true;
	pending_ = p;
	return true;
}

// **Z10c: A HELD ENVELOPE OUTLIVES A RESET** (the lead, 2026-10-18). A host's reset, a re-activation and a new Resolution (which
// is a latency restart in the host) all reset the STFT, which used to drop a freeze silently — the persist rule broken inside
// one session. So each re-arms the pending copy from the last published block, the same road a load takes: the next block
// primes it at the stop now in force (the block is in dBFS) if Freeze is on, and drops it if not. A pending load is not
// overwritten; a block that held nothing re-arms nothing that holds.
void SpectralDevice::rearmFreeze() noexcept {
	if (pending_.armed) {
		return;
	}
	std::array<float, kFreezeWords> words {};
	if (freezeSnapshot_.read(words.data(), words.size()) == 0) {
		return; // nothing published yet: a fresh instance has nothing of its own to bring back
	}
	(void)restoreFreeze(words.data(), words.size());
}

void SpectralDevice::takePendingFreeze() noexcept {
	if (!pending_.armed) {
		return;
	}
	pending_.armed = false;
	// ON LOAD, by design: Freeze ON primes the stored envelope, frozen from this block; Freeze OFF ignores it.
	if (!target_.vocodeFreeze) {
		return;
	}
	for (int k = 0; k < 2; ++k) {
		if (pending_.held[static_cast<size_t>(k)]) {
			stft_.primeFrozen(k, pending_.dbfs[static_cast<size_t>(k)].data(), pending_.flatness[static_cast<size_t>(k)]);
		}
	}
	frozenTracked_ = pending_.tracked;
	frozenNoteSemis_ = pending_.tracked ? pending_.noteSemis : frozenNoteSemis_;
}

void SpectralDevice::setReadingsSink(device::ReadingsSink sink) noexcept {
	sink_ = sink;
	stft_.setAnalyse(sink.publish != nullptr);
}

void SpectralDevice::publish() noexcept {
	if (sink_.publish == nullptr) {
		return;
	}
	const double f0 = shaperF0(target_, target_.keyTrack, noteHz_);
	const bool playing = target_.keyTrack && noteSeen_;
	const bool sounding = stft_.sidechainSounding();
	const bool vocoding = stft_.vocodeBusy();
	if (stft_.analysedForTest() != published_ || f0 != publishedF0_ || playing != publishedPlaying_
	    || sounding != publishedSounding_ || vocoding) {
		publishedSounding_ = sounding;
		published_ = stft_.analysedForTest();
		publishedF0_ = f0;
		publishedPlaying_ = playing;
		std::array<float, kReadingCount> values {};
		std::copy_n(stft_.bands(), kSpectrumBands, values.begin());
		values[kReadingF0] = static_cast<float>(f0);
		values[kReadingPlaying] = playing ? 1.0f : 0.0f;
		values[kReadingSideSounding] = sounding ? 1.0f : 0.0f;
		// THE SIDECHAIN'S ENVELOPE at the bands' centres, in dBFS (the FFT's magnitude scaled as the bands are), while vocoding.
		const auto& grid = stft_.sidechainEnvelope();
		const double toFs = 20.0 * std::log10(3.141592653589793 / std::max(1, stft_.fftSize()));
		for (int b = 0; b < kSpectrumBands; ++b) {
			float db = kBandFloorDb;
			if (vocoding) {
				const double pos = std::clamp((std::log2(bandCentreHz(b)) - std::log2(kEnvelopeLowHz)) / kEnvelopeOctaves * (kEnvelopePoints - 1), 0.0,
				                              static_cast<double>(kEnvelopePoints - 1));
				const auto g = std::min(static_cast<int>(pos), kEnvelopePoints - 2);
				const double t = pos - g;
				const double e = grid[static_cast<size_t>(g)] * (1.0 - t) + grid[static_cast<size_t>(g + 1)] * t;
				db = static_cast<float>(std::max(static_cast<double>(kBandFloorDb), e + toFs));
			}
			values[kReadingSideEnvelope + static_cast<size_t>(b)] = db;
		}
		sink_.publish(sink_.context, values.data(), values.size());
	}
}

void SpectralDevice::refresh() noexcept {
	if (!dirty_ || k_.empty()) {
		return;
	}
	dirty_ = false;
	const bool identity = binGains(target_, target_.keyTrack, noteHz_, stft_.sampleRate(), stft_.fftSize(),
	                               BinScratch { k_.data(), hz_.data(), base_.data() }, stft_.gains());
	gainsIdentity_ = identity;
	// FORMANT SHIFT'S RATIO PER BIN, from the shared function (shaper::formantRatios), the drawer's controls read as the device's.
	const bool fshift = target_.enabled && target_.fshiftOn;
	stft_.setFormantShift(fshift);
	if (fshift) {
		shaper::ShaperParams p;
		p.formantShift.on = true;
		p.formantShift.lowSemitones = target_.fshiftLow;
		p.formantShift.highSemitones = target_.fshiftHigh;
		p.formantShift.crossoverHz = static_cast<float>(std::exp2(static_cast<double>(target_.fshiftCrossover)));
		p.formantShift.link = target_.fshiftLink;
		const int bins = stft_.binCount();
		for (int i = 0; i < bins; ++i) {
			hz_[static_cast<size_t>(i)] = static_cast<float>(std::max(static_cast<double>(i) * stft_.sampleRate() / stft_.fftSize(), shaper::kMinHz));
		}
		shaper::formantRatios(shaper::prepare(p, shaper::kKeyTrackPivotHz), hz_.data(), bins, stft_.formantRatios());
	}
	stft_.setMix(target_.mix);
}

void SpectralDevice::processStereo(float* left, float* right, int frames) noexcept {
	refresh();
	// THE IDENTITY, A BLOCK AT A TIME: the operators' curve may be the identity while the vocoder acts or fades (it depends on
	// the sidechain this block brought), and then frames must run.
	// THE OPERATORS' f0 (the note raised by Offset) moves the operators and the harmonic layouts; THE SQUARE CARRIER FOLLOWS THE
	// NOTE ONLY (Z9, Requirement: never Offset) — Key Track's note, or C3 with it off.
	stft_.setF0Hz(shaperF0(target_, target_.keyTrack, noteHz_));
	stft_.setCarrierHz(target_.keyTrack ? noteHz_ : kFixedF0Hz);
	takePendingFreeze();
	trackFreeze();
	stft_.setIdentity(gainsIdentity_ && !stft_.vocodeBusy() && !stft_.formantShiftOn() && !stft_.partialsOn());
	if (target_.enabled) {
		fx::scrubNonFinite(left, right, frames);
	}
	stft_.process(left, right, frames);
	publishFreeze();
	publish();
}

void SpectralDevice::processMono(float* mono, int frames) noexcept {
	refresh();
	stft_.setF0Hz(shaperF0(target_, target_.keyTrack, noteHz_));
	stft_.setCarrierHz(target_.keyTrack ? noteHz_ : kFixedF0Hz);
	takePendingFreeze();
	trackFreeze();
	stft_.setIdentity(gainsIdentity_ && !stft_.vocodeBusy() && !stft_.formantShiftOn() && !stft_.partialsOn());
	if (target_.enabled) {
		fx::scrubNonFinite(mono, nullptr, frames);
	}
	stft_.process(mono, nullptr, frames);
	publishFreeze();
	publish();
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
SpectralDevice& asDevice(void* storage) { return *static_cast<SpectralDevice*>(storage); }

Settings settingsOf(const float* fields, size_t fieldsPresent) {
	Row row {};
	const size_t take = std::min(fieldsPresent, row.size());
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	return stateOf(row, take);
}
} // namespace

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	const auto& device = *static_cast<const SpectralDevice*>(storage);
	return index == 0 ? device.readFreeze(out, capacity) : index == 1 ? device.readCurve(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 0 ? asDevice(storage).restoreFreeze(in, n) : index == 1 && asDevice(storage).restoreCurve(in, n);
}

bool applyBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 1 && asDevice(storage).applyCurve(in, n);
}

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(SpectralDevice),
    .stateAlign = alignof(SpectralDevice),
    .construct = [](void* storage) { new (storage) SpectralDevice(); },
    .destruct = [](void* storage) { asDevice(storage).~SpectralDevice(); },
    .prepare = [](void* storage, double sampleRate, int) { asDevice(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asDevice(storage).reset(); },
    .setFields = [](void* storage, const float* fields,
                    size_t fieldsPresent) { asDevice(storage).setTarget(settingsOf(fields, fieldsPresent)); },
    // THE NOTE ROAD: Key Track's f0. On the bus it is the last note the host hands every device.
    .setNoteContext = [](void* storage, const device::NoteContext& note) { asDevice(storage).setNoteContext(note); },
    .processStereo = [](void* storage, float* left, float* right,
                        int frames) { asDevice(storage).processStereo(left, right, frames); },
    .processMono = [](void* storage, float* mono, int frames) { asDevice(storage).processMono(mono, frames); },
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asDevice(storage).setReadingsSink(sink); },
    // THE STORED ROW AND THE RATE DECIDE IT (L1), and nothing else: not Enabled (L3), not an operator, not Mix.
    .latencySamples = [](const void*, const float* fields, size_t fieldsPresent,
                         double sampleRate) { return latencyFor(settingsOf(fields, fieldsPresent).resolution, sampleRate); },
    // THE VOCODER'S MODULATOR (design §10): the plugin's sidechain, on the bus road.
    .setSidechain = [](void* storage, const float* left, const float* right, int, bool sounding) {
	    asDevice(storage).setSidechain(left, right, sounding);
    },
    // THE DRAWN CURVE, the hand's block: pulled after setFields, bounded, one attempt (device::HandBlock).
    .pullHandBlocks = [](void* storage, const void* source) { asDevice(storage).pullCurve(*static_cast<const SpectralDevice*>(source)); },
};

} // namespace feathers::fx::spectral
