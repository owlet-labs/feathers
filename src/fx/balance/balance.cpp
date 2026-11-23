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

#include "fx/balance/balance_types.h"

#include "fx/owltt/comb_split.h"
#include "fx/wet_highpass.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>

// STEREO BALANCE'S RENDER PATH — see `balance_types.h` for the law. One host sample at a time, so a host that cuts a block anywhere gets the same bits.
//
// **NO ALLOCATION AFTER `prepare`**: the comb lines and the sinc table are built there, and nothing else is.
// **DETECTOR STATE IS NOT PERSISTED**: the smoothed levels are derived from the audio and rebuild within a Release; the controls are the whole patch.

namespace feathers::fx::balance {

namespace {

/// A NON-FINITE SAMPLE BECOMES 0 — the one treatment every guard in the loop uses.
[[nodiscard]] inline float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

/// THE FLOOR UNDER THE DETECTOR'S MEAN SQUARES, so a band that is silent on both sides reads as balanced (-120 dBFS rms) and never as a ratio of noise.
constexpr float kEnergyFloor = 1.0e-12f;
/// 20 / ln(10): nepers of amplitude to decibels.
constexpr float kNeperDb = 8.685889638065035f;
/// THE HIGHPASS IS HELD UNDER THIS SHARE OF THE RATE, as the other devices' is.
constexpr double kHpNyquistFraction = 0.45;
/// THE PARAMETERS' (Strength, Pan) AND THE ENABLE'S SMOOTHING, seconds.
constexpr double kSmoothSeconds = 0.005;
constexpr double kEnableSeconds = 0.010;
/// THE CONTROL TICK THE SPACINGS ARE ADVANCED ON, samples.
constexpr int kTickFrames = 32;
/// THE DETECTOR'S MEAN-SQUARE WINDOW, seconds.
constexpr double kMeanSeconds = 0.030;

struct OpsBalance {
	Settings state {};
	double sampleRate = 48000.0;

	std::unique_ptr<owltt::CombSplit> comb;
	fx::WetHighpass hp;
	bool hpOn = false;
	bool subThru = false;

	struct Detector {
		/// THE POWER IN A 30 ms WINDOW (so a sine's ripple at twice its frequency is not what the ballistics follow), then the Attack / Release
		/// smoothing of that.
		float mL = 0.0f;
		float mR = 0.0f;
		float eL = 0.0f;
		float eR = 0.0f;
		/// THE SAME TWO LAGS ON THE BAND'S OUTPUT SIDES, for the meter's "after".
		float aL = 0.0f;
		float aR = 0.0f;
		float oL = 0.0f;
		float oR = 0.0f;
		float d = 0.0f;
		SideGains gains {};
		float level = 0.0f;
	};
	std::array<Detector, 3> band {};
	float meanCoef = 0.0f;
	float attackCoef = 0.0f;
	float releaseCoef = 0.0f;
	int bandsApplied = 3;

	// THE COMB'S SPACINGS, glided in log2 over 5 ms and ramped across a block, as OWLTT's are.
	std::array<double, 2> log2Fc {};
	std::array<double, 2> delayStart {};
	int untilTick = 0;
	std::array<double, 2> delayEnd {};
	bool primed = false;
	std::array<double, 2> spacingHz {};

	bool heardNote = false;
	double noteSemis = 60.0;

	float strengthNow = 0.5f;
	float panNow = 0.0f;
	float focusNow = 0.0f;
	float toleranceNow = 0.0f;
	float widenNow = 0.0f;
	bool smoothPrimed = false;
	float act = 1.0f; ///< the enable crossfade, 0 (dry) .. 1 (balanced)
	bool cleared = false;
	uint32_t tailLeft = 0;
	uint32_t tailSamples = 0;

	device::ReadingsSink sink {};

	void resetDetectors() {
		for (Detector& d : band) {
			d = Detector {};
		}
	}

	/// THE COMB'S SPACINGS FOR THE NEXT CONTROL TICK: Low and High Crossover as tooth spacings (`owltt::combSpacingHz`; tracking, the knobs are the note times their
	/// offsets), glided in log2 over 5 ms, clamped to what the lines serve, turned into delays. The delay ramps linearly across the tick, so a spacing never steps.
	/// **THE TICK IS A FIXED NUMBER OF SAMPLES, counted across calls**: how a host cuts its blocks is not a setting, and the glide must not depend on it.
	void advanceTick() {
		const double lo = owltt::CombSplit::minSpacing(sampleRate);
		const double hi = owltt::CombSplit::maxSpacing(sampleRate);
		const bool tracking = state.keyTrack && heardNote;
		const double noteHz = owltt::keyHzOf(noteSemis);
		const double alpha = 1.0 - std::exp(-static_cast<double>(kTickFrames) / (owltt::kKeySmoothSeconds * std::max(sampleRate, 1.0)));
		for (size_t k = 0; k < 2; ++k) {
			const double knob = static_cast<double>(k == 0 ? state.spacing : state.spacing2);
			const double knobHz = owltt::trackedCrossoverHz(knob, tracking, noteHz);
			const double goal = std::log2(std::clamp(owltt::combSpacingHz(knobHz, k == 1), lo, hi));
			if (!primed) {
				log2Fc[k] = goal;
				delayEnd[k] = owltt::CombSplit::delayFor(std::exp2(goal), sampleRate);
			}
			else {
				log2Fc[k] += alpha * (goal - log2Fc[k]);
				if (std::fabs(goal - log2Fc[k]) < owltt::kGlideSnap) {
					log2Fc[k] = goal;
				}
			}
			delayStart[k] = delayEnd[k];
			delayEnd[k] = owltt::CombSplit::delayFor(std::exp2(log2Fc[k]), sampleRate);
			spacingHz[k] = std::exp2(log2Fc[k]);
		}
		primed = true;
	}

	void clearState() {
		if (comb) {
			comb->clear();
		}
		hp.reset();
		resetDetectors();
		primed = false;
		untilTick = 0;
		tailLeft = 0;
	}

	void latch() {
		const double lo = fx::kWetHighpassLog2Min;
		hpOn = static_cast<double>(state.hpWet) > lo + device::kOffEndSlack;
		const double hz = std::clamp(std::exp2(static_cast<double>(state.hpWet)), std::exp2(lo), std::exp2(fx::kWetHighpassLog2Max));
		hp.set(fx::highpassAt(hpOn, hpOn ? hz : std::exp2(lo), sampleRate, kHpNyquistFraction));
		subThru = state.subThru;
		if (!hpOn) {
			hp.reset();
		}
		const double rate = std::max(sampleRate, 1.0);
		meanCoef = static_cast<float>(1.0 - std::exp(-1.0 / (rate * kMeanSeconds)));
		attackCoef = static_cast<float>(1.0 - std::exp(-1.0 / (rate * std::exp2(static_cast<double>(state.attack)))));
		releaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (rate * std::exp2(static_cast<double>(state.release)))));
	}

	void process(float* left, float* right, int frames) {
		if (frames <= 0) {
			return;
		}
		if (!comb || !comb->prepared()) {
			return; // not prepared: a pass-through, never an allocation here
		}
		const bool wantOn = state.enabled;
		if (!wantOn && act <= 0.0f) {
			if (!cleared) {
				clearState();
				cleared = true;
			}
			publish();
			return;
		}
		cleared = false;
		if (state.bands != bandsApplied) {
			bandsApplied = state.bands;
			resetDetectors();
		}
		if (!smoothPrimed) {
			strengthNow = state.strength;
			panNow = state.pan;
			focusNow = state.focus;
			toleranceNow = state.toleranceDb;
			widenNow = state.widen;
			smoothPrimed = true;
		}

		const owltt::CombShape shape { state.sharp ? 2 : 1 };
		const size_t nb = state.bands >= 3 ? 3 : 2;
		const float smoothCoef = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(sampleRate, 1.0) * kSmoothSeconds)));
		const float enableStep = static_cast<float>(1.0 / (std::max(sampleRate, 1.0) * kEnableSeconds));

		for (int i = 0; i < frames; ++i) {
			const float inL = finiteOr0(left[i]);
			const float inR = finiteOr0(right[i]);
			// A FEEDFORWARD COMB'S TAIL ENDS AFTER ITS LONGEST DELAY OF EXACT ZEROS; that is what `holdsTail` counts.
			if (inL != 0.0f || inR != 0.0f) {
				tailLeft = tailSamples;
			}
			else if (tailLeft > 0) {
				--tailLeft;
			}
			strengthNow += (state.strength - strengthNow) * smoothCoef;
			if (std::fabs(state.strength - strengthNow) < 1.0e-6f) {
				strengthNow = state.strength;
			}
			panNow += (state.pan - panNow) * smoothCoef;
			if (std::fabs(state.pan - panNow) < 1.0e-6f) {
				panNow = state.pan;
			}
			focusNow += (state.focus - focusNow) * smoothCoef;
			if (std::fabs(state.focus - focusNow) < 1.0e-6f) {
				focusNow = state.focus;
			}
			toleranceNow += (state.toleranceDb - toleranceNow) * smoothCoef;
			if (std::fabs(state.toleranceDb - toleranceNow) < 1.0e-6f) {
				toleranceNow = state.toleranceDb;
			}
			widenNow += (state.widen - widenNow) * smoothCoef;
			if (std::fabs(state.widen - widenNow) < 1.0e-6f) {
				widenNow = state.widen;
			}

			// HP WET FIRST: the split and the detectors hear the high-passed signal; the lows come back after, Sub Thru willing.
			float wl = inL;
			float wr = inR;
			float lowL = 0.0f;
			float lowR = 0.0f;
			if (hpOn) {
				if (subThru) {
					lowL = hp.taken(0, inL);
					lowR = hp.taken(1, inR);
				}
				wl = hp.wet(0, inL);
				wr = hp.wet(1, inR);
			}

			if (untilTick <= 0) {
				advanceTick();
				untilTick = kTickFrames;
			}
			const double t = static_cast<double>(kTickFrames - untilTick + 1) / static_cast<double>(kTickFrames);
			--untilTick;
			const float d1 = static_cast<float>(delayStart[0] + (delayEnd[0] - delayStart[0]) * t);
			const float d2 = static_cast<float>(delayStart[1] + (delayEnd[1] - delayStart[1]) * t);
			std::array<float, 3> bl {};
			std::array<float, 3> br {};
			if (nb == 3) {
				const auto l = comb->process(0, wl, d1, d2, shape);
				const auto r = comb->process(1, wr, d1, d2, shape);
				bl = { l.between, l.partB, l.partA };
				br = { r.between, r.partB, r.partA };
			}
			else {
				const float onL = comb->processFirst(0, wl, d1, shape);
				const float onR = comb->processFirst(1, wr, d1, shape);
				bl = { wl - onL, onL, 0.0f };
				br = { wr - onR, onR, 0.0f };
			}

			// THE DETECTORS FIRST, for every band: the whole's imbalance needs the summed energies.
			float totalL = 0.0f;
			float totalR = 0.0f;
			for (size_t b = 0; b < nb; ++b) {
				Detector& s = band[b];
				s.mL += meanCoef * (bl[b] * bl[b] - s.mL);
				s.mR += meanCoef * (br[b] * br[b] - s.mR);
				s.eL += (s.mL > s.eL ? attackCoef : releaseCoef) * (s.mL - s.eL);
				s.eR += (s.mR > s.eR ? attackCoef : releaseCoef) * (s.mR - s.eR);
				if (s.mL < 1.0e-30f) s.mL = 0.0f;
				if (s.mR < 1.0e-30f) s.mR = 0.0f;
				if (s.eL < 1.0e-30f) s.eL = 0.0f;
				if (s.eR < 1.0e-30f) s.eR = 0.0f;
				s.d = 0.5f * std::log((s.eL + kEnergyFloor) / (s.eR + kEnergyFloor));
				totalL += s.eL;
				totalR += s.eR;
			}
			const float dTotal = 0.5f * std::log((totalL + kEnergyFloor) / (totalR + kEnergyFloor));
			// THE CORRECTION (`bandCorrection`: the whole's part, then Focus of each band's own, outside the Tolerance), each band's gains kept for the meters.
			std::array<float, 3> cl {};
			std::array<float, 3> cr {};
			std::array<float, 3> energy {};
			const float tolNepers = toleranceNow / kNeperDb;
			for (size_t b = 0; b < nb; ++b) {
				Detector& s = band[b];
				s.gains = bandCorrection(s.d, dTotal, strengthNow, focusNow, tolNepers);
				cl[b] = bl[b] * s.gains.left;
				cr[b] = br[b] * s.gains.right;
				energy[b] = 0.5f * (s.gains.left * s.gains.left * s.eL + s.gains.right * s.gains.right * s.eR);
			}
			// WIDEN: the pair's mids move between the channels (mono-safe), weighted by the corrected energies so the sides stay level. Off, nothing is touched.
			if (widenNow != 0.0f) {
				const size_t first = nb == 3 ? 1 : 0;
				const size_t second = first + 1;
				const WidenAmounts a = widenAmounts(widenNow, energy[first], energy[second]);
				const float mFirst = 0.5f * (cl[first] + cr[first]);
				const float mSecond = 0.5f * (cl[second] + cr[second]);
				cl[first] += a.first * mFirst;
				cr[first] -= a.first * mFirst;
				cl[second] -= a.second * mSecond;
				cr[second] += a.second * mSecond;
			}
			float sumL = 0.0f;
			float sumR = 0.0f;
			for (size_t b = 0; b < nb; ++b) {
				Detector& s = band[b];
				sumL += cl[b];
				sumR += cr[b];
				// WHAT THE METER SHOWS AFTER: the band's own output sides, measured as the input's are.
				s.aL += meanCoef * (cl[b] * cl[b] - s.aL);
				s.aR += meanCoef * (cr[b] * cr[b] - s.aR);
				s.oL += (s.aL > s.oL ? attackCoef : releaseCoef) * (s.aL - s.oL);
				s.oR += (s.aR > s.oR ? attackCoef : releaseCoef) * (s.aR - s.oR);
				if (s.aL < 1.0e-30f) s.aL = 0.0f;
				if (s.aR < 1.0e-30f) s.aR = 0.0f;
				if (s.oL < 1.0e-30f) s.oL = 0.0f;
				if (s.oR < 1.0e-30f) s.oR = 0.0f;
			}
			float outL = sumL + lowL;
			float outR = sumR + lowR;
			if (panNow != 0.0f) {
				const SideGains p = panGains(panNow);
				outL *= p.left;
				outR *= p.right;
			}
			if (act < 1.0f || !wantOn) {
				act = wantOn ? std::min(1.0f, act + enableStep) : std::max(0.0f, act - enableStep);
				outL = inL + act * (outL - inL);
				outR = inR + act * (outR - inR);
			}
			left[i] = outL;
			right[i] = outR;
		}
		for (size_t b = 0; b < nb; ++b) {
			band[b].level = std::sqrt(0.5f * (band[b].eL + band[b].eR));
		}
		publish();
	}

	void publish() {
		if (sink.publish == nullptr) {
			return;
		}
		std::array<float, static_cast<size_t>(Reading::kCount)> v {};
		for (size_t b = 0; b < 3; ++b) {
			const Detector& s = band[b];
			const bool live = b < static_cast<size_t>(state.bands);
			v[static_cast<size_t>(bandReading(b, 0))] = live ? s.d * kNeperDb : 0.0f;
			v[static_cast<size_t>(bandReading(b, 1))] = live ? 0.5f * std::log((s.oL + kEnergyFloor) / (s.oR + kEnergyFloor)) * kNeperDb : 0.0f;
			v[static_cast<size_t>(bandReading(b, 2))] = live ? s.level : 0.0f;
		}
		v[static_cast<size_t>(Reading::Spacing1Hz)] = static_cast<float>(spacingHz[0]);
		v[static_cast<size_t>(Reading::Spacing2Hz)] = static_cast<float>(spacingHz[1]);
		v[static_cast<size_t>(Reading::NoteHz)] = state.keyTrack && heardNote ? static_cast<float>(owltt::keyHzOf(noteSemis)) : 0.0f;
		sink.publish(sink.context, v.data(), v.size());
	}
};

OpsBalance& asOps(void* storage) { return *static_cast<OpsBalance*>(storage); }
const OpsBalance& asOps(const void* storage) { return *static_cast<const OpsBalance*>(storage); }

} // namespace

float imbalanceForTest(const void* storage, size_t band) { return asOps(storage).band[std::min<size_t>(band, 2)].d; }
SideGains gainsForTest(const void* storage, size_t band) { return asOps(storage).band[std::min<size_t>(band, 2)].gains; }
float strengthNowForTest(const void* storage) { return asOps(storage).strengthNow; }

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsBalance),
    .stateAlign = alignof(OpsBalance),
    .construct = [](void* storage) {
	    new (storage) OpsBalance();
	    asOps(storage).latch();
    },
    .destruct = [](void* storage) { asOps(storage).~OpsBalance(); },
    .prepare =
        [](void* storage, double sampleRate, int) {
	        OpsBalance& ops = asOps(storage);
	        ops.sampleRate = std::max(sampleRate, 1.0);
	        ops.comb = std::make_unique<owltt::CombSplit>();
	        ops.comb->prepare(ops.sampleRate);
	        ops.tailSamples = static_cast<uint32_t>(owltt::CombSplit::kLineSize) + 64u;
	        ops.act = ops.state.enabled ? 1.0f : 0.0f;
	        ops.clearState();
	        ops.cleared = false;
	        ops.smoothPrimed = false;
	        ops.latch();
        },
    .reset =
        [](void* storage) {
	        OpsBalance& ops = asOps(storage);
	        ops.clearState();
	        ops.heardNote = false;
	        ops.cleared = false;
	        ops.smoothPrimed = false;
	        ops.act = ops.state.enabled ? 1.0f : 0.0f;
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        OpsBalance& ops = asOps(storage);
	        ops.state = stateOf(row, take);
	        ops.latch();
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        // LATCHED ONLY WHILE A KEY IS HELD, so the combs keep the last note after its release (OWLTT's and the Phaser's rule).
	        if (!note.gate || !std::isfinite(note.pitchSemitones)) {
		        return;
	        }
	        OpsBalance& ops = asOps(storage);
	        ops.heardNote = true;
	        ops.noteSemis = static_cast<double>(note.pitchSemitones);
        },
    .processStereo = [](void* storage, float* left, float* right, int frames) { asOps(storage).process(left, right, frames); },
    .processMono = nullptr,
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asOps(storage).sink = sink; },
    .holdsTail = [](const void* storage) { return asOps(storage).tailLeft > 0; },
};

} // namespace feathers::fx::balance
