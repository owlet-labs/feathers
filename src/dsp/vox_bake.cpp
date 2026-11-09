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

#include "dsp/vox_bake.h"

#include "dsp/character_bank.h"

#include <algorithm>
#include <cmath>

// The firmware's renderPhiVox, restated as a bake. Read the header for the parity boundary; the
// comments here say which line of the original each piece is, so the two can be diffed by eye.

namespace feathers {

namespace {

/// The last 1/32 of a cycle, where the release lives — the firmware's `toWrap < (1u << 27)` against
/// a 32-bit phase, which is the same fraction written as one.
constexpr double kReleaseFraction = 1.0 / 32.0;

/// One raised-cosine pulse, sin²(π·φ) over one formant period — the firmware's
/// `0x3FFFFFFF - (cosv >> 1)` in the units this file works in.
double raisedCosine(double formantPhase) {
	return 0.5 * (1.0 - std::cos(2.0 * M_PI * formantPhase));
}

} // namespace

void bakeVoxCycle(const VoxParams& params, double fundamentalHz, VoxCycle& out) {
	out = VoxCycle {};
	if (!(fundamentalHz > 0.0) || !std::isfinite(fundamentalHz)) {
		// Not a note. An unvoiced cycle rather than a divide — the consumer skips the road entirely.
		return;
	}

	if (!voxParamsVoiced(params)) {
		// Every gain zero: the bake is a flat line, and saying so lets the blend skip it.
		return;
	}
	out.voiced = true;

	// PERIODS PER CYCLE, per formant: the one thing the note is used for, and the whole of the
	// note-dependence this engine has.
	std::array<double, kVoxNumFormants> periodsPerCycle {};
	for (size_t f = 0; f < kVoxNumFormants; ++f) {
		periodsPerCycle[f] = static_cast<double>(params.formant[f].frequencyHz) / fundamentalHz;
	}

	for (int slot = 0; slot <= kVoxSlots; ++slot) {
		// The wrap guard repeats slot 0 rather than evaluating phase 1.0, which IS phase 0 — a cycle
		// is periodic and an evaluator reading slot 256 must get what it would get at the seam.
		const int at = slot == kVoxSlots ? 0 : slot;
		const double phase = static_cast<double>(at) / static_cast<double>(kVoxSlots);

		double sample = 0.0;
		for (size_t f = 0; f < kVoxNumFormants; ++f) {
			// How far into the burst this cycle-phase stands, in formant periods.
			const double elapsed = periodsPerCycle[f] * phase;
			// THE INDEX STICKS AT THE LAST SLOT rather than running off the end — the firmware's
			// `std::min(..., kPhiVoxMaxPulses - 1)`. It reads as a silent tail rather than a repeat
			// because the gains are zero-padded past the burst, which is the builder's promise; the
			// clamp is what makes that promise safe rather than a bounds bug.
			const auto index = static_cast<size_t>(
			    std::min(std::floor(elapsed), static_cast<double>(kVoxMaxPulses - 1)));
			const double within = elapsed - std::floor(elapsed);
			sample += static_cast<double>(params.formant[f].pulseGain[index]) * raisedCosine(within);
		}

		// CYCLE-END RELEASE, the firmware's hard-won fix and the reason it is not optional: when the
		// burst is longer than the note period the wrap AMPUTATES a pulse mid-flight, a discontinuity
		// every single cycle — it buzzes. A linear fade over the last 1/32 lands every truncation on
		// zero. It multiplies BEFORE the DC subtraction, as it does there, so the compensation is not
		// faded along with the signal it is compensating.
		const double toWrap = 1.0 - phase;
		if (toWrap < kReleaseFraction) {
			sample *= toWrap / kReleaseFraction;
		}

		out.slots[static_cast<size_t>(slot)] = static_cast<float>(sample);
	}

	// DC REMOVAL, BY MEASUREMENT — and this is a DELIBERATE DIVERGENCE from the firmware, which
	// computes the offset from a formula instead. The formula is `0.5 * sum(gains)` scaled by
	// min(Tformant/Tnote, 1), and it is right exactly while the burst FITS INSIDE THE CYCLE. When it
	// does not, the pulses past the wrap never sound — the cycle only ever contains
	// periodsPerCycle of them — but the formula subtracts all of them anyway. Measured on this bake:
	// twelve pulses at 1200 Hz over a 200 Hz note, where six periods fit, leaves the whole cycle
	// sitting at -0.31. That is not a rounding difference, it is a third of full scale of standing
	// offset, and long bursts at low formant rates are ordinary country rather than a corner.
	//
	// A BAKE CAN SIMPLY MEASURE, which is the thing a per-sample stream could not do — the firmware
	// needs a closed form because it never has the cycle in front of it, and we always do. So the
	// mean of the slots we just wrote is subtracted from the slots we just wrote: exact by
	// construction, correct whatever the burst does, and it absorbs the release fade's own
	// contribution for free, which no formula written against the gains could.
	//
	// The wrap guard is excluded from the mean and corrected with the rest: it is a repeat of slot
	// 0, so counting it would weight one sample twice.
	double mean = 0.0;
	for (int slot = 0; slot < kVoxSlots; ++slot) {
		mean += static_cast<double>(out.slots[static_cast<size_t>(slot)]);
	}
	mean /= static_cast<double>(kVoxSlots);
	for (float& value : out.slots) {
		value = static_cast<float>(static_cast<double>(value) - mean);
	}
}

bool voxParamsVoiced(const VoxParams& params) {
	for (const VoxFormant& formant : params.formant) {
		for (const float gain : formant.pulseGain) {
			if (gain != 0.0f) {
				return true;
			}
		}
	}
	return false;
}

VoxCharacter buildVoxFor(const Character& c) {
	VoxCharacter out;
	const size_t n = std::min<size_t>(c.count, kMaxSounds);

	// PASS ONE — is there a vowel here at all, and whose. Presence is a weighted sum of non-negative
	// triangle reads, so a zero total is every sound reading zero and there is nothing for pass two
	// to blend. This is what keeps the vowel-free terrain costing one extra triangle per sound
	// rather than a whole recipe per sound.
	//
	// PRESENCE BLENDS BY WEIGHT ALONE AND IS NOT NORMALISED — it is the house field rule (the
	// operator authors blend the same way, Σ w·field(coordinate) and never field(Σ w·coordinate)),
	// and it is the quantity that SHOULD fall off when a vowel-free neighbour is in the mix.
	std::array<float, kMaxSounds> identityWeight {};
	float presence = 0.0f;
	float identityTotal = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		const SoundCoord& s = c.sounds[i];
		const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
		const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		const MorphBasis morphFrame =
		    sliceFrameFor<kMorphTriangles>(rotateFor(pin), home, s.residue.morph);
		// DECISION V7'S COORDINATE, through the engine's own name for it. The species RATE is derived
		// from kFlagshipFeedbackRate, so the field belongs to the operator ladder and is read the
		// way that ladder is read — travel-coupled, turning with the plane, residue-protected. The
		// consequence worth stating: rotating a sound's plane moves its vowel by the MORPH plane's
		// rules and banks the difference in the morph residue. The vox basis and the vox residue
		// rows are for the twelve vox triangles inside buildVoxSliced, which is a different
		// question and is answered separately below.
		const double lead = flagshipLeadCoordinate(morphFrame, home, s.travelX, s.travelY);
		const auto w = static_cast<float>(s.weight);
		const float here = voxPresenceAt(lead);
		presence += w * here;
		// THE IDENTITY WEIGHT IS w × presence, and this is the distinction the whole function turns
		// on: PRESENCE DILUTES, IDENTITY DOES NOT. buildVoxSliced answers every position, including
		// the three quarters of the terrain with no vowel in them, where its formant numbers mean
		// nothing whatever. Weighting the recipe by w alone would let a vowel-free sound drag a real
		// vowel's formants halfway toward arbitrary values — so standing between an "ah" and a plain
		// morph sound would give you neither an "ah" nor a fade, but a third, arbitrary vowel. A
		// sound with no vowel must contribute nothing to what the vowel IS while still making it
		// quieter, which is exactly w × presence for the identity and w alone for the presence.
		identityWeight[i] = w * here;
		identityTotal += identityWeight[i];
	}
	out.presence = presence;
	if (!(identityTotal > 0.0f)) {
		// No sound here has a vowel to lend its identity to. Nothing to blend, nothing to normalise,
		// and no division to guard.
		return out;
	}

	// PASS TWO — the recipe, blended at the identity weights and normalised by their sum.
	//
	// BLENDING THE PARAMS, NOT THE CYCLES, and the alternative was seriously considered and
	// rejected: crossfading N baked vox WAVEFORMS gives you two singers fading past each other,
	// while interpolating the FORMANTS gives you one singer moving their mouth. The intermediate
	// between "ah" and "ee" is not a phantom value — it is a DIPHTHONG, which is what a real vocal
	// tract produces when it moves between two shapes, and it is the point of this engine. The
	// phantom-value argument recorded at buildFlagshipFor does not transfer here: it is about a
	// feedback depth, where the space between two values contains sounds neither neighbour has,
	// and vowel space is continuous and traversable. Do not "fix" this back into a crossfade.
	//
	// FREQUENCIES BLEND IN LOG SPACE. A tract moving 300 → 900 Hz passes through the geometric
	// middle, and the layer feeding this already thinks in octaves — the wander triangles are
	// exp2 of their triangle reads. Linear-Hz interpolation would race away from the low vowel and
	// crawl into the high one, which is audibly not how a mouth moves. PULSE GAINS BLEND LINEARLY:
	// they are amplitudes of the glottal burst, not positions in a vowel space.
	// ANCHORED AT THE FIRST CONTRIBUTING SOUND, and NORMALISED BY DIVISION rather than by a
	// reciprocal, both for the same reason: A SINGLE SOUND MUST COME BACK AS ITSELF, TO THE BIT.
	// That is the position-purity claim the whole character layer makes, and a log round trip does
	// not have it — exp2(log2(x)) is within a ULP of x, not equal to it, and multiplying by 1/total
	// is not exactly 1 even when the weight IS the total. Anchoring makes the lone sound's term
	// log2(ref/ref), which is exactly zero, so exp2 of it is exactly one; dividing makes its weight
	// exactly one, so its gains pass through untouched. The anchor is also the better-conditioned
	// arithmetic for a real blend, since the logs stay near zero instead of near ten.
	std::array<float, kVoxNumFormants> anchorHz {};
	bool haveAnchor = false;
	std::array<float, kVoxNumFormants> logRatio {};
	for (size_t i = 0; i < n; ++i) {
		if (!(identityWeight[i] > 0.0f)) {
			// A sound with no vowel contributes nothing to the identity — and skipping it is also
			// what keeps the logarithm safe, since a position whose params were never built carries
			// a formant frequency of zero and log2(0) would poison the sum with −inf.
			continue;
		}
		const SoundCoord& s = c.sounds[i];
		const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
		const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		const VoxBasis voxFrame = sliceFrameFor<kVoxTriangles>(rotateFor(pin), home, s.residue.vox);
		const AngleWarpScope likes(c.likes.of(s.slot)); // this sound's phase warp on its vowel sites (dsp/phase_warp.h)
		const VoxParams p =
		    buildVoxSliced(s.zone, s.phaseOffset, s.travelX, s.travelY, voxFrame, s.gammaBase);
		const float w = identityWeight[i] / identityTotal;
		if (!haveAnchor) {
			for (size_t f = 0; f < kVoxNumFormants; ++f) {
				anchorHz[f] = std::max(p.formant[f].frequencyHz, kVoxMinFormantHz);
			}
			haveAnchor = true;
		}
		for (size_t f = 0; f < kVoxNumFormants; ++f) {
			const float hz = std::max(p.formant[f].frequencyHz, kVoxMinFormantHz);
			logRatio[f] += w * std::log2(hz / anchorHz[f]);
			for (size_t k = 0; k < kVoxMaxPulses; ++k) {
				out.params.formant[f].pulseGain[k] += w * p.formant[f].pulseGain[k];
			}
		}
		out.params.breath += w * p.breath;
		out.params.articulation += w * p.articulation;
	}
	for (size_t f = 0; f < kVoxNumFormants; ++f) {
		out.params.formant[f].frequencyHz = anchorHz[f] * std::exp2(logRatio[f]);
	}
	// The gains could in principle cancel to nothing across a blend, and a bake of that would be a
	// flat line — so the flag is taken from what was actually blended rather than from presence.
	out.audible = voxParamsVoiced(out.params);
	return out;
}

float voxPresenceAt(double coordinate) {
	// freqMult 1.0f, as every field over the space passes: the rate half of a slice belongs to a
	// sound's travel, and a landscape is not travelling.
	return owlet::dsp::phi::evalTriangle(coordinate, 1.0f, kVoxSpeciesField);
}

float evalVoxCycle(const VoxCycle& cycle, double phase01) {
	// Wrapped rather than clamped: a cycle is periodic, and a caller handing over 1.5 means half a
	// cycle on, not the end of the table.
	double p = phase01 - std::floor(phase01);
	const double scaled = p * static_cast<double>(kVoxSlots);
	auto index = static_cast<size_t>(scaled);
	// Defensive against a phase that rounds to exactly kVoxSlots after the multiply, which the
	// subtraction above cannot quite rule out at the top of the range.
	index = std::min(index, static_cast<size_t>(kVoxSlots - 1));
	const auto frac = static_cast<float>(scaled - static_cast<double>(index));
	const float a = cycle.slots[index];
	const float b = cycle.slots[index + 1];
	return a + (b - a) * frac;
}

} // namespace feathers
