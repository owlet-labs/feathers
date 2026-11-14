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

#include "fx/harm/harm.h"
#include "fx/scrub.h"

#include "fx/harm/harm_types.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <numbers>
#include <new>

namespace feathers::fx::harm {

namespace {

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	return index < std::size(kParamDescs) ? &kParamDescs[index] : nullptr;
}

/// CLAMPED AND ROUNDED BY THE CONTROL'S OWN DECLARATION — the one rule every write here obeys, and
/// the same one ParamSet::setBase applies on the parameter road. A stepped control rounds to an
/// integer index and a continuous one merely clamps; a non-finite value is refused outright, because
/// this is the last gate before a number becomes something the sample loop runs on.
float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	double v = value;
	if (desc.stepped) {
		v = std::round(v);
	}
	v = std::clamp(v, desc.min, desc.max);
	return static_cast<float>(v);
}

/// log2(seconds) as declared, to seconds as the loop wants them.
///
/// THE MINIMUM MEANS NO TIME AT ALL, which is the convention device.h states for LogSeconds: a
/// logarithmic domain has no zero to mean it with, so the bottom of the travel carries the meaning.
/// An attack at its minimum is instant rather than half a millisecond, and a porta at its minimum
/// snaps rather than gliding imperceptibly.
float secondsOf(float log2Seconds) {
	return log2Seconds <= static_cast<float>(kTimeLog2Min) ? 0.0f : std::exp2(log2Seconds);
}

float log2Of(float seconds) {
	return seconds <= 0.0f ? static_cast<float>(kTimeLog2Min) : std::log2(seconds);
}

} // namespace

std::string harmonicLabel(size_t index) {
	if (index >= kHarmonicCount) {
		return {};
	}
	const HarmonicRatio& r = kHarmonics[index];
	std::string out = std::to_string(static_cast<int>(r.num)) + ":"
	                  + std::to_string(static_cast<int>(r.den));

	// A RATIO BELOW UNISON IS NAMED BY ITS RECIPROCAL, PLUS "down". Folding a descending ratio UP the
	// way intervalOf does is arithmetically exact and reads terribly: 1:3 comes out as "2 oct down +
	// perfect 4th" — correct (−2400 + 498 = −1902) and not how anyone says it. A musician says "an
	// octave and a fifth down", which is the reciprocal, 3:1, negated.
	//
	// IT ALSO NAMES MORE OF THEM. Eight of this table's nine sub-unison entries have a clean
	// reciprocal name; folding upward names five, and names them awkwardly.
	const bool descending = r.num < r.den;
	// THE FLIPPED PAIR, ASKED DIRECTLY rather than looked up as another row. The first version
	// searched the table for the reciprocal and gave up when it was absent — which lost 5:8, whose
	// reciprocal 8:5 is a plain minor sixth and simply has no row of its own. A ratio does not need
	// to be in the table to have a name.
	const Interval interval =
	    descending ? intervalOfRatio(static_cast<int>(r.den), static_cast<int>(r.num))
	               : intervalOfRatio(static_cast<int>(r.num), static_cast<int>(r.den));
	// AN OCTAVE COUNT WITH AN UNNAMEABLE REMAINDER IS WORSE THAN NOTHING — "1 oct + something"
	// invites the reader to supply the something, and the ratio already said it exactly.
	if (interval.name == nullptr) {
		return out;
	}

	std::string said;
	if (interval.octaves != 0) {
		said = std::to_string(std::abs(interval.octaves)) + " oct";
	}
	if (std::string_view(interval.name) != "unison") {
		if (!said.empty()) {
			said += " + ";
		}
		said += interval.name;
	}
	else if (said.empty()) {
		said = "unison";
	}
	if (descending) {
		said += " down";
	}
	return out + "  ·  " + said;
}

Row rowOf(const State& state) {
	// THE FIELD ORDER, WRITTEN ONCE. Index-aligned with Param and therefore with kParamDescs, which
	// is what lets a host walk this row against the declarations without a second table.
	//
	// THE PER-RENDER FIELDS ARE NOT HERE, and their absence is the point: noteSemitones,
	// bendSemitones and gate are supplied by whoever runs HARM, exactly as the filter's
	// keyOffsetSemitones is. A row carrying them would be claiming to know something only the caller
	// knows, and a patch storing them would restore a note that is not being played.
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = state.settings.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Harmonic)] = static_cast<float>(state.settings.harmonic);
	row[static_cast<size_t>(Param::Level)] = state.settings.level;
	row[static_cast<size_t>(Param::Fine)] = state.settings.fineSemitones;
	row[static_cast<size_t>(Param::Phase)] = state.settings.phase;
	row[static_cast<size_t>(Param::Spread)] = state.settings.spread;
	row[static_cast<size_t>(Param::Attack)] = log2Of(state.settings.attackSeconds);
	row[static_cast<size_t>(Param::Release)] = log2Of(state.settings.releaseSeconds);
	row[static_cast<size_t>(Param::Width)] = state.settings.width;
	row[static_cast<size_t>(Param::Depth)] = state.settings.depth;
	row[static_cast<size_t>(Param::Porta)] = log2Of(state.settings.portaSeconds);
	row[static_cast<size_t>(Param::Poly)] = state.poly ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::DepthFollowsEnv)] = state.settings.depthFollowsEnv ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Mode)] = state.settings.highpass ? 1.0f : 0.0f;
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
	row[static_cast<size_t>(Param::Partial1Active)] = state.settings.active ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Delay)] = log2Of(state.settings.delaySeconds);
	row[static_cast<size_t>(Param::Hold)] = log2Of(state.settings.holdSeconds);
	row[static_cast<size_t>(Param::Decay)] = log2Of(state.settings.decaySeconds);
	row[static_cast<size_t>(Param::Sustain)] = state.settings.sustain;
	for (int k = 1; k < kPartialCount; ++k) {
		const PartialSettings& p = state.settings.more[static_cast<size_t>(k - 1)];
		const auto at = [k](PartialField f) { return static_cast<size_t>(partialParam(k, f)); };
		row[at(PartialField::Harmonic)] = static_cast<float>(p.harmonic);
		row[at(PartialField::Level)] = p.level;
		row[at(PartialField::Fine)] = p.fineSemitones;
		row[at(PartialField::Phase)] = p.phase;
		row[at(PartialField::Spread)] = p.spread;
		row[at(PartialField::Width)] = p.width;
		row[at(PartialField::Depth)] = p.depth;
		row[at(PartialField::Mode)] = p.highpass ? 1.0f : 0.0f;
		row[at(PartialField::Active)] = p.active ? 1.0f : 0.0f;
	}
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		// A FIELD THE PAYLOAD DOES NOT REACH KEEPS ITS DECLARED DEFAULT — the rule device_row.h
		// states, and the reason the count travels with the row rather than beside it.
		return static_cast<size_t>(p) < fieldsPresent
		           ? coerce(*d, row[static_cast<size_t>(p)])
		           : static_cast<float>(d->def);
	};

	out.settings.enabled = read(Param::Enabled) >= 0.5f;
	out.settings.harmonic = static_cast<size_t>(read(Param::Harmonic));
	out.settings.level = read(Param::Level);
	out.settings.fineSemitones = read(Param::Fine);
	out.settings.phase = read(Param::Phase);
	out.settings.spread = read(Param::Spread);
	out.settings.attackSeconds = secondsOf(read(Param::Attack));
	out.settings.releaseSeconds = secondsOf(read(Param::Release));
	out.settings.width = read(Param::Width);
	out.settings.depth = read(Param::Depth);
	out.settings.portaSeconds = secondsOf(read(Param::Porta));
	out.poly = read(Param::Poly) >= 0.5f;
	out.settings.depthFollowsEnv = read(Param::DepthFollowsEnv) >= 0.5f;
	out.settings.highpass = read(Param::Mode) >= 0.5f;
	out.settings.active = read(Param::Partial1Active) >= 0.5f;
	out.settings.delaySeconds = secondsOf(read(Param::Delay));
	out.settings.holdSeconds = secondsOf(read(Param::Hold));
	out.settings.decaySeconds = secondsOf(read(Param::Decay));
	out.settings.sustain = read(Param::Sustain);
	for (int k = 1; k < kPartialCount; ++k) {
		PartialSettings& p = out.settings.more[static_cast<size_t>(k - 1)];
		const auto at = [k, &read](PartialField f) { return read(partialParam(k, f)); };
		p.harmonic = static_cast<size_t>(at(PartialField::Harmonic));
		p.level = at(PartialField::Level);
		p.fineSemitones = at(PartialField::Fine);
		p.phase = at(PartialField::Phase);
		p.spread = at(PartialField::Spread);
		p.width = at(PartialField::Width);
		p.depth = at(PartialField::Depth);
		p.highpass = at(PartialField::Mode) >= 0.5f;
		p.active = at(PartialField::Active) >= 0.5f;
	}
	// noteSemitones, bendSemitones and gate are deliberately untouched — see rowOf.
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	// READ OFF THE ROW rather than off a second switch over the same fields — the row IS this
	// device's indexed view of itself, and a switch beside it would be a second copy of the order
	// rowOf exists to hold alone.
	return rowOf(state)[index];
}

void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	const device::ParamDesc* desc = descOf(param);
	if (desc == nullptr || !std::isfinite(value)) {
		return;
	}
	// THROUGH THE ROW, so there is one field order and one coercion rule. A write past a control's
	// end STOPS AT THE END rather than falling through to the default — right for a hand on a knob,
	// and the trap the DAHDSR's setValueOf records having fallen into.
	//
	// The per-render fields survive the trip because stateOf leaves them alone and this preserves
	// them explicitly: a knob edit must not silently retune the device to middle C.
	const Settings live = state->settings;
	Row row = rowOf(*state);
	row[index] = coerce(*desc, value);
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
	state->settings.noteSemitones = live.noteSemitones;
	state->settings.bendSemitones = live.bendSemitones;
	state->settings.gate = live.gate;
}

double oscillatorHz(const Settings& s) {
	// note + fine + bend is a pitch in semitones; the harmonic ratio multiplies the frequency.
	//
	// A4 = 440 Hz at semitone 69, which is the plugin's own convention everywhere else.
	const double semis = static_cast<double>(s.noteSemitones) + static_cast<double>(s.fineSemitones)
	                     + static_cast<double>(s.bendSemitones);
	const double fundamental = 440.0 * std::exp2((semis - 69.0) / 12.0);
	return fundamental * static_cast<double>(ratioAt(s.harmonic));
}

double notchCentreHz(const Settings& s) {
	// THE COUPLING THAT MAKES HARM ONE DEVICE. The notch sits where the oscillator sits — so moving
	// the harmonic to a fifth moves the notch to that fifth, and the thing being stripped is always
	// the thing being replaced.
	//
	// AND IT SITS THERE WHATEVER THE LEVEL SAYS. This used to fall back to the fundamental at Level 0,
	// citing the firmware's `isOscEnabled() ? ratio : 1.0f` — but that test is on the HARMONIC selector
	// having an OFF position (harmonic == 0), which this device's control does not have; the firmware
	// never consults level in its notch, so turning Level down there leaves the notch on the selected
	// harmonic. Ours slid it back to 1:1 (HARM parity sweep, 2026-09-16). A notch on a harmonic nobody
	// is sounding is what the firmware does, and what a hand turning Level down to hear the strip alone
	// is asking for.
	return oscillatorHz(s);
}

double notchDepth(const Settings& s) {
	return std::clamp(static_cast<double>(s.depth), 0.0, 1.0);
}

double playedNoteHz(const Settings& s) {
	// NOTE PLUS BEND, WITHOUT FINE. See harm_types.h: fine detunes the OSCILLATOR, and an axis whose
	// origin moved when the oscillator was detuned could not show the detune at all.
	const double semis = static_cast<double>(s.noteSemitones) + static_cast<double>(s.bendSemitones);
	return 440.0 * std::exp2((semis - 69.0) / 12.0);
}

double notchRatio(const Settings& s) {
	const double note = playedNoteHz(s);
	if (!(note > 0.0)) {
		return 1.0; // no note is no axis; 1:1 is the only answer that is not a guess
	}
	// THE DIVISION IS THE DEFINITION. Restating the harmonic lookup here would be a second
	// derivation of the same quantity, and the level<=0 fallback is exactly the kind of thing two
	// derivations stop agreeing about.
	return notchCentreHz(s) / note;
}

double notchMagnitudeAtRatio(const Settings& s, double ratio) {
	// HIGH-PASS MODE DRAWS THE BRICKWALL (backlog NL), on the same axis by the same argument: its shape is a
	// function of ratio / f0 alone, and f0 is the notch's own centre. Width and Depth do not reach it.
	if (s.highpass) {
		const double f0 = notchRatio(s);
		return f0 > 0.0 && ratio > 0.0 ? feathers::dsp::kEllipticHighpass.magnitudeAtRatio(ratio / f0) : 1.0;
	}
	if (s.width <= 0.0f) {
		return 1.0; // width zero is no notch at all — refreshCoefficients' rule, not a second one
	}
	const double centre = notchRatio(s);
	if (!(centre > 0.0) || !(ratio > 0.0)) {
		return 1.0;
	}
	// THE ANALOG PROTOTYPE, which is what makes this axis note-independent: |H| depends on
	// ratio/centre and Q, and on nothing that has a sample rate in it.
	//
	//   H(jw) = (w0^2 - w^2 + jw*w0*(1-depth)/Q) / (w0^2 - w^2 + jw*w0/Q)
	//
	// divided through by w0^2 with r = w/w0. The numerator carries the depth exactly as the biquad's
	// alphaZ does, so at r = 1 this is (1-depth) and the picture agrees with Param::Depth's promise
	// by derivation rather than by coincidence.
	const double r = ratio / centre;
	const double q = std::max(notchQ(s), 0.01);
	const double depth = notchDepth(s);
	const double real = 1.0 - r * r;
	const double imagNumerator = r * (1.0 - depth) / q;
	const double imagDenominator = r / q;
	const double numerator = real * real + imagNumerator * imagNumerator;
	const double denominator = real * real + imagDenominator * imagDenominator;
	if (!(denominator > 0.0)) {
		return 1.0;
	}
	return std::sqrt(numerator / denominator);
}

void ratioProfile(const Settings& s, float* ratioOut, float* gainOut, size_t n) {
	if (gainOut == nullptr || n == 0) {
		return;
	}
	if (n == 1) {
		// ONE POINT IS THE LOW END, not the midpoint — the loop below divides by n-1 and a single
		// point has no span to divide. Stated rather than left to a division by zero.
		if (ratioOut != nullptr) {
			ratioOut[0] = static_cast<float>(kProfileLoRatio);
		}
		gainOut[0] = static_cast<float>(notchMagnitudeAtRatio(s, kProfileLoRatio));
		return;
	}
	const double step = std::log2(kProfileHiRatio / kProfileLoRatio) / static_cast<double>(n - 1);
	for (size_t i = 0; i < n; ++i) {
		const double ratio = kProfileLoRatio * std::exp2(step * static_cast<double>(i));
		if (ratioOut != nullptr) {
			ratioOut[i] = static_cast<float>(ratio);
		}
		gainOut[i] = static_cast<float>(notchMagnitudeAtRatio(s, ratio));
	}

	// AND ONE POINT LANDS EXACTLY ON THE CENTRE, which is the whole reason this is not just a loop.
	//
	// A TRUE NULL IS INFINITELY NARROW AND NO SAMPLING FINDS IT. MEASURED across every harmonic in
	// the table at width 0.02 (Q = 29.4), full depth, taking the deepest point the curve contains:
	//
	//     96 points  -1.3 dB      1024 points  -16.1 dB
	//    256 points  -5.4 dB      2048 points  -22.0 dB
	//    601 points -12.0 dB      4096 points  -28.0 dB
	//
	// The truth is minus infinity, so EVERY one of those is wrong, and raising the count buys 6 dB
	// per doubling forever. The filter's card answered its own version of this with resolution
	// (kPointsPerPixel, filter_card.h) because a resonant PEAK is finite; a notch is not, and the
	// same answer here would be an arbitrarily large number that is still wrong.
	//
	// SO THE ONE POINT THAT MATTERS IS PLACED RATHER THAN HOPED FOR. The centre's location is known
	// exactly — it is notchRatio — so the nearest sample is moved onto it. The trough is then
	// `1 - depth` EXACTLY at every width and every harmonic, by construction, and the substituted
	// point has moved by at most half a step — MEASURED at 0.0064 of an octave across the whole
	// harmonic table at 601 points, where the step is 0.0133, which is 0.16 px on a 200 px axis.
	const double centre = notchRatio(s);
	// THE BRICKWALL HAS NO NULL TO PLACE — its stopband is equiripple, not a point.
	if (!s.highpass && centre >= kProfileLoRatio && centre <= kProfileHiRatio) {
		const double exact = std::log2(centre / kProfileLoRatio) / step;
		const auto nearest = static_cast<size_t>(std::llround(exact));
		if (nearest < n) {
			if (ratioOut != nullptr) {
				ratioOut[nearest] = static_cast<float>(centre);
			}
			gainOut[nearest] = static_cast<float>(notchMagnitudeAtRatio(s, centre));
		}
	}
}

double notchQ(const Settings& s) {
	// INVERTED, BECAUSE THE CONTROL IS WIDTH AND Q IS SHARPNESS. The firmware computes
	// `30 - ((hpf-1)/126) * 29.5` over its 1..127 knob; normalised to 0..1 that is `30 - width*29.5`,
	// so width 0 is Q=30 (surgical) and width 1 is Q=0.5 (wide).
	//
	// The name is the whole reason this is worth a function: the firmware calls this knob `hpf` and
	// its comment calls it a cutoff, and a port that trusted either would have produced a plausible
	// number doing the wrong job.
	const double width = std::clamp(static_cast<double>(s.width), 0.0, 1.0);
	return 30.0 - width * 29.5;
}


//------------------------------------------------------------------------------------------------//
// THE DSP.                                                                                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// A one-pole smoothing coefficient for a target reached in `seconds`, or 0 for "snap".
///
/// STATED AS A TIME CONSTANT rather than as the firmware's per-block retention. The firmware mapped
/// its 1..127 knob onto a rate and then raised it to the block length, which makes the glide depend
/// on the host's buffer size — the exact class of bug tests/test_synth.cpp's blocks-of-1-and-37 case
/// exists to catch. Our porta is declared in SECONDS, so the coefficient is per sample and the block
/// cannot reach it.
double smoothingCoefficient(double seconds, double sampleRate) {
	if (seconds <= 0.0 || sampleRate <= 0.0) {
		return 0.0;
	}
	return std::exp(-1.0 / (seconds * sampleRate));
}

} // namespace

void Harm::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	reset();
}

void Harm::reset() {
	for (Voice& v : voices_) {
		v.reset();
		v.wasActive = false;
	}
	envelope_.reset();
	gateWas_ = false;
	gridPhase_ = 0;
}

void Harm::refreshCoefficients(Voice& v, const Settings& p) {
	// WIDTH ZERO IS NO NOTCH AT ALL, which is the control's declared meaning and not a small-Q
	// approximation of it. A notch at Q=30 still removes a real amount of signal, so a device at its
	// DEFAULT settings would colour a sound it was asked to leave alone — the identity guarantee the
	// Level control also carries.
	//
	// `p` IS THE PARTIAL, seen as the first (`partialSlice`): its harmonic, width, depth and mode, with the note and the gate
	// the whole device shares. Everything below is what the single notch always did, once per active partial.
	const bool wasOn = v.notchOn;
	// **DEPTH FOLLOWS THE ENVELOPE, WHEN ASKED** (`Param::DepthFollowsEnv`): the depth is scaled by
	// where the note envelope stands at this grid point, so the notch fades in over Attack and out over Release.
	// Exactly 1 when the option is off, and `x * 1.0` is `x`, so that road is today's bits. An envelope at ZERO
	// is the cheap case: the notch takes the width-zero road below — no biquad runs, and its state is cleared on
	// the way out so a note brings it back from silence. Read at the grid, so with Attack at zero the notch
	// engages at the first grid point after the gate rather than on the gate's sample (at most kControlBlock late).
	// **HIGH-PASS MODE (backlog NL): THE BRICKWALL RUNS INSTEAD, at the frequency the oscillator is actually at.**
	// `currentHz` rather than the target, so Porta glides the corner with the oscillator — the coupling the
	// header promises. Width, Depth and the envelope do not reach it (the brickwall has one shape), so it is
	// always on in this mode. Each filter's state is cleared on the way OUT, for the notch's reason below:
	// switching back must not replay a tail from whenever that filter last ran.
	const bool wasHighpass = v.highpassOn;
	v.highpassOn = p.highpass;
	if (v.highpassOn) {
		if (v.notchOn) {
			v.left.reset();
			v.right.reset();
		}
		v.notchOn = false;
		v.notchDepthNow = 0.0;
		const double f0 = v.currentHz > 0.0 ? v.currentHz : notchCentreHz(p);
		v.hpLeft.setCorner(f0, sampleRate_);
		v.hpRight.setCorner(f0, sampleRate_);
		return;
	}
	if (wasHighpass) {
		v.hpLeft.reset();
		v.hpRight.reset();
	}
	const double envelopeDepth = p.depthFollowsEnv ? envelope_.level() : 1.0;
	v.notchOn = p.width > 0.0f && envelopeDepth > 0.0;
	v.notchDepthNow = 0.0;
	if (!v.notchOn) {
		// AND THE BIQUAD FORGETS WHERE IT WAS (backlog EH, shape 6): a stage that keeps its state across
		// a bypass replays it on the way back in — a Q=30 notch holding a tail from whenever Width was
		// last up rang it into the first samples after Width came back. The bus utility's DC stage names
		// the same precedent. Cleared on the way OUT, so the way back in starts silent.
		if (wasOn) {
			v.left.reset();
			v.right.reset();
		}
		return;
	}

	// THE CENTRE TRACKS THE OSCILLATOR, which is the coupling that makes HARM one device — and falls
	// back to the fundamental when the oscillator is silent, because a notch tracking a harmonic
	// nobody is sounding would be stripping a frequency for no reason. Both live in notchCentreHz.
	const double centre = notchCentreHz(p);
	// BOUNDED BY NYQUIST HERE, where the sample rate is known. The control declares a musical range
	// and the DSP owns its own limit — the same division the filter's cutoff draws.
	const double nyquist = sampleRate_ * 0.5;
	const double fc = std::clamp(centre, 1.0, nyquist * 0.98);
	const double q = std::max(notchQ(p), 0.01);

	// RBJ's notch, verbatim in shape: a zero pair exactly on the unit circle at w0, so the centre is
	// removed rather than attenuated, and the poles inside it by alpha.
	const double w0 = 2.0 * std::numbers::pi * fc / sampleRate_;
	const double cosW0 = std::cos(w0);
	const double alpha = std::sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	// THE ZEROS COME INSIDE THE CIRCLE BY THE DEPTH. RBJ's notch puts them exactly ON it — which is
	// why the centre was removed outright and why no partial cut was expressible. Giving the
	// NUMERATOR its own alpha is the whole change: at depth 1, alphaZ is 0 and these are the
	// coefficients that shipped; at depth 0 the numerator equals the denominator termwise and the
	// filter is a wire.
	//
	// THE POLES DO NOT MOVE, and that is the property worth stating: `a1`, `a2` and `a0` are
	// untouched, so the notch keeps its WIDTH while its DEPTH changes. A depth that moved the poles
	// would be a second width control wearing another name.
	v.notchDepthNow = notchDepth(p) * envelopeDepth;
	const double alphaZ = alpha * (1.0 - v.notchDepthNow);
	v.b0 = (1.0 + alphaZ) / a0;
	v.b1 = -2.0 * cosW0 / a0;
	v.b2 = (1.0 - alphaZ) / a0;
	v.a1 = -2.0 * cosW0 / a0;
	v.a2 = (1.0 - alpha) / a0;
}

void Harm::advanceControl(Voice& v, const Settings& p, bool retriggered) {
	// PORTAMENTO ON THE TRACKED FREQUENCY, and the notch rides it because the notch is recomputed
	// from the same number. A glide that moved the oscillator and left the notch behind would pull
	// the two halves of the device apart for the length of the glide.
	const double targetHz = oscillatorHz(p);
	// `retriggered` IS LATCHED BY THE CALLER, ON THE GRID, NOT AT THE END OF THE CALL. It used to be written after the whole
	// buffer, so a 256-frame block saw "retriggered" at sixteen grid points and a 1-frame block at
	// one — harmless while the trigger only snapped a frequency that was already at its target, and a
	// block-size dependence the moment the trigger also aimed the phase (HARM parity sweep,
	// 2026-09-16; the block-size case caught it at 1e-6). One latch serves every partial.
	const double coefficient = smoothingCoefficient(p.portaSeconds, sampleRate_);
	if (!v.haveFrequency || retriggered || coefficient == 0.0) {
		// SNAP ON RETRIGGER, so a new note starts AT its pitch. A porta that applied to the first note
		// of a phrase would bend into every attack, which is not what a portamento control means.
		v.currentHz = targetHz;
		v.haveFrequency = true;
	}
	else if (v.currentHz != targetHz) {
		const double retention = std::pow(coefficient, static_cast<double>(kControlBlock));
		v.currentHz = targetHz + (v.currentHz - targetHz) * retention;
	}

	// PHASE IS A START PHASE, so a retrigger AIMS the oscillator at it — and converges rather than
	// snapping, which clicks. The firmware computes the signed error to the target on the trigger and
	// pays it off at `error * increment / 16` per sample (~16 cycles; its comment says ~24). The port
	// added Phase as a constant offset to a free-running accumulator, so only the first note after a
	// reset started where the knob said (HARM parity sweep, 2026-09-16). The offset itself is applied
	// at the read, so the target for the accumulator is zero: the error is the accumulator's own
	// distance from a whole turn, shortest way round.
	if (retriggered) {
		const double away = v.phase - std::floor(v.phase + 0.5); // in (-0.5, 0.5]: how far past the nearest turn
		v.catchup = -away;
	}

	// PINK-NOISE SCALING OF THE SINE, -3 dB/octave above 40 Hz and unity below, recomputed with the
	// tracked frequency (the firmware's `cachedPinkQ31`, recomputed when the pitch moves).
	v.pinkScale = v.currentHz > 40.0 ? std::sqrt(40.0 / v.currentHz) : 1.0;

	refreshCoefficients(v, p);
	++v.gridPoints;
}

void Harm::processStereo(float* left, float* right, int frames) {
	// THIS CALL'S WINDOWS, taken before any return so none outlives its call.
	const std::array<Runs, kPartialCount> runs = pending_;
	pending_ = {};
	bool anyWindow = false;
	for (const Runs& r : runs) {
		anyWindow = anyWindow || r.level != nullptr || r.phase != nullptr || r.spread != nullptr;
	}
	windowedCalls_ += anyWindow ? 1u : 0u;
	if (left == nullptr || frames <= 0) {
		return;
	}
	// THE BYPASS IS A RETURN, not a pass through a silent oscillator and an inert notch. Not one
	// sample is read or written, which is what lets a chain holding a switched-off HARM render the
	// same bits it rendered before the device was added — the filter's own rule, and the reason its
	// bypass is spelled the same way.
	//
	// AND THE STATE GOES QUIET WITH IT. A device switched out mid-phrase must not keep its envelope
	// open or its notch ringing, or switching it back on would resume a note nobody is playing.
	if (!target_.enabled) {
		bool live = envelope_.level() > 0.0;
		for (const Voice& v : voices_) {
			live = live || v.haveFrequency;
		}
		if (live) {
			reset();
		}
		return;
	}
	// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 before anything sees it (as EQ and Comp do; `fx::scrubNonFinite`).
	fx::scrubNonFinite(left, right, frames);

	// THE PARTIALS THAT ARE IN THE SOUND, each seen as the first (`partialSlice`). AN INACTIVE PARTIAL IS NOT RENDERED AT
	// ALL — no notch, no sine, no grid point — and is reset the moment it goes out, so switching it back on starts it
	// from silence rather than replaying whatever it last held.
	std::array<Settings, kPartialCount> slice;
	std::array<int, kPartialCount> live {};
	int liveCount = 0;
	for (int k = 0; k < kPartialCount; ++k) {
		slice[static_cast<size_t>(k)] = partialSlice(target_, k);
		Voice& v = voices_[static_cast<size_t>(k)];
		if (slice[static_cast<size_t>(k)].active) {
			live[static_cast<size_t>(liveCount++)] = k;
			v.wasActive = true;
		}
		else if (v.wasActive) {
			v.reset();
			v.wasActive = false;
		}
	}

	// SPREAD IS A HALF TURN AT THE TOP, and it is separate from Phase — the firmware folded the two
	// into one control and could not offer a centred oscillator with any width at all.
	std::array<double, kPartialCount> level {};
	std::array<double, kPartialCount> spreadTurns {};
	std::array<double, kPartialCount> phaseOffset {};
	for (int n = 0; n < liveCount; ++n) {
		const auto k = static_cast<size_t>(live[static_cast<size_t>(n)]);
		level[k] = static_cast<double>(std::clamp(slice[k].level, 0.0f, 1.0f));
		spreadTurns[k] = static_cast<double>(std::clamp(slice[k].spread, 0.0f, 1.0f)) * 0.5;
		phaseOffset[k] = static_cast<double>(std::clamp(slice[k].phase, 0.0f, 1.0f));
	}

	int done = 0;
	while (done < frames) {
		// THE GRID IS ABSOLUTE, carried in gridPhase_ across calls, so where a host's block boundary
		// falls cannot change which samples the coefficients were updated on.
		if (gridPhase_ == 0) {
			const bool retriggered = target_.gate && !gateWas_;
			gateWas_ = target_.gate;
			for (int n = 0; n < liveCount; ++n) {
				const auto k = static_cast<size_t>(live[static_cast<size_t>(n)]);
				advanceControl(voices_[k], slice[k], retriggered);
			}
		}
		const int take = std::min(frames - done, kControlBlock - gridPhase_);

		// THE ENVELOPE'S COEFFICIENTS, once per chunk like the attack and release always were.
		EnvelopeCoefs env;
		env.attack = smoothingCoefficient(target_.attackSeconds, sampleRate_);
		env.release = smoothingCoefficient(target_.releaseSeconds, sampleRate_);
		env.decay = smoothingCoefficient(target_.decaySeconds, sampleRate_);
		env.delaySamples = static_cast<int>(std::lround(std::max(0.0f, target_.delaySeconds) * sampleRate_));
		env.holdSamples = static_cast<int>(std::lround(std::max(0.0f, target_.holdSeconds) * sampleRate_));
		env.sustain = static_cast<double>(std::clamp(target_.sustain, 0.0f, 1.0f));

		std::array<double, kPartialCount> increment {};
		for (int n = 0; n < liveCount; ++n) {
			const auto k = static_cast<size_t>(live[static_cast<size_t>(n)]);
			increment[k] = voices_[k].currentHz / sampleRate_;
			voices_[k].samplesRendered += static_cast<size_t>(take);
		}

		for (int i = 0; i < take; ++i) {
			const int at = done + i;

			// THE ENVELOPE MOVES PER SAMPLE, not per grid: it is what a note's attack sounds like, and
			// a sixteen-sample staircase on it is audible in a way a sixteen-sample staircase on a
			// notch's centre is not. (The release is floored to zero inside `Envelope`: a one-pole release never
			// reaches zero on its own, so without it the sine's gain went denormal after every note and stayed
			// there — on the audio thread. HARM parity sweep, 2026-09-16.)
			const double envelope = envelope_.step(target_.gate, env);

			// STRIP FIRST, THEN REPLACE. The order is the device: the notches take their harmonics out of
			// what arrived — in series, partial 1 first — and the oscillators put theirs back.
			float l = left[at];
			float r = right != nullptr ? right[at] : l;
			for (int n = 0; n < liveCount; ++n) {
				Voice& v = voices_[static_cast<size_t>(live[static_cast<size_t>(n)])];
				if (v.notchOn) {
					l = v.left.process(l, v.b0, v.b1, v.b2, v.a1, v.a2);
					r = v.right.process(r, v.b0, v.b1, v.b2, v.a1, v.a2);
				}
				else if (v.highpassOn) {
					l = v.hpLeft.process(l);
					r = v.hpRight.process(r);
				}
			}

			for (int n = 0; n < liveCount; ++n) {
				const auto k = static_cast<size_t>(live[static_cast<size_t>(n)]);
				const Voice& v = voices_[k];
				const Runs& run = runs[k];
				// PER SAMPLE WHILE A ROUTED SOURCE MOVES THEM (backlog FK), by the same clamps as the scalars above.
				const double levelAt = run.level != nullptr ? static_cast<double>(std::clamp(run.level[at], 0.0f, 1.0f)) : level[k];
				if (levelAt > 0.0 && envelope > 0.0) {
					const double spreadAt =
					    run.spread != nullptr ? static_cast<double>(std::clamp(run.spread[at], 0.0f, 1.0f)) * 0.5 : spreadTurns[k];
					const double phaseAt = run.phase != nullptr ? static_cast<double>(std::clamp(run.phase[at], 0.0f, 1.0f)) : phaseOffset[k];
					// PINK-SCALED: -3 dB per octave above 40 Hz, never a boost — the firmware's `pinkScale`,
					// which the port had dropped (HARM parity sweep, 2026-09-16). Harmonic spans 1:8 to 32:1,
					// so without it 32:1 on middle C sat a full 23 dB above where the reference puts it.
					// SPREAD IS SYMMETRIC about Phase, +-spread/2, as the firmware places it (`targetPhaseL =
					// spread/2, targetPhaseR = -spread/2`); the port held L and moved R by the whole spread —
					// the same angle between them, a different place for both.
					// AND FULL LEVEL IS 0 dBFS — DECIDED (backlog EM). The firmware's sine leaves
					// its fixed-point chain ~36 dB down (`>>6`, then `<<1`, `<<2`), a headroom artefact rather
					// than a level anyone chose; the parity sweep asked and this is the answer.
					const double gain = levelAt * envelope * v.pinkScale;
					l += static_cast<float>(
					    gain * std::sin(2.0 * std::numbers::pi * (v.phase + phaseAt - 0.5 * spreadAt)));
					r += static_cast<float>(
					    gain * std::sin(2.0 * std::numbers::pi * (v.phase + phaseAt + 0.5 * spreadAt)));
				}
			}

			left[at] = l;
			if (right != nullptr) {
				right[at] = r;
			}

			for (int n = 0; n < liveCount; ++n) {
				const auto k = static_cast<size_t>(live[static_cast<size_t>(n)]);
				Voice& v = voices_[k];
				v.phase += increment[k];
				// THE CATCH-UP, paid off a sixteenth of the increment's share per sample; the last crumb is
				// paid whole so the error reaches exactly zero rather than asymptoting into denormals.
				if (v.catchup != 0.0) {
					double correction = v.catchup * increment[k] * (1.0 / 16.0);
					if (std::fabs(correction) >= std::fabs(v.catchup) || std::fabs(correction) < 1.0e-9) {
						correction = v.catchup;
					}
					v.phase += correction;
					v.catchup -= correction;
				}
				if (v.phase >= 1.0 || v.phase < 0.0) {
					v.phase -= std::floor(v.phase);
				}
			}
		}

		done += take;
		gridPhase_ = (gridPhase_ + take) % kControlBlock;
	}
}

void Harm::processMono(float* mono, int frames) {
	// ONE CHANNEL IS THE STEREO PATH WITH NO RIGHT, rather than a second copy of the loop — the
	// spread simply has nowhere to go. Written this way because two loops is two places for the
	// envelope's shape to drift apart.
	processStereo(mono, nullptr, frames);
}

namespace {

Harm& asHarm(void* storage) { return *static_cast<Harm*>(storage); }

} // namespace

double opsCurrentHzForTest(const void* storage, int partial) {
	return static_cast<const Harm*>(storage)->currentHzForTest(partial);
}

const Settings& opsTargetForTest(const void* storage) {
	return static_cast<const Harm*>(storage)->target();
}

size_t opsWindowedCallsForTest(const void* storage) { return static_cast<const Harm*>(storage)->windowedCalls_; }

double opsNotchDepthForTest(const void* storage, int partial) {
	return static_cast<const Harm*>(storage)->notchDepthForTest(partial);
}

double opsHighpassCornerForTest(const void* storage, int partial) {
	return static_cast<const Harm*>(storage)->highpassCornerForTest(partial);
}

size_t opsPartialSamplesForTest(const void* storage, int partial) {
	return static_cast<const Harm*>(storage)->partialSamplesForTest(partial);
}

size_t opsPartialGridPointsForTest(const void* storage, int partial) {
	return static_cast<const Harm*>(storage)->partialGridPointsForTest(partial);
}

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`. Every partial's Level, Phase and Spread.
constexpr std::array<uint8_t, 3 * kPartialCount> kWindowedControls = [] {
	std::array<uint8_t, 3 * kPartialCount> out {};
	for (int k = 0; k < kPartialCount; ++k) {
		out[static_cast<size_t>(3 * k + 0)] = static_cast<uint8_t>(partialParam(k, PartialField::Level));
		out[static_cast<size_t>(3 * k + 1)] = static_cast<uint8_t>(partialParam(k, PartialField::Phase));
		out[static_cast<size_t>(3 * k + 2)] = static_cast<uint8_t>(partialParam(k, PartialField::Spread));
	}
	return out;
}();

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(Harm),
    .stateAlign = alignof(Harm),
    .construct = [](void* storage) { new (storage) Harm(); },
    .destruct = [](void* storage) { asHarm(storage).~Harm(); },
    .prepare = [](void* storage, double sampleRate, int) { asHarm(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asHarm(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        // THE PER-RENDER FIELDS SURVIVE THE ROW, which is why this reads the live settings back
	        // rather than assigning what stateOf produced. A row carries patch state only; the note
	        // and the gate arrive on the other road and a decode must not clear them.
	        const Settings live = asHarm(storage).target();
	        Settings settings = stateOf(row, take).settings;
	        settings.noteSemitones = live.noteSemitones;
	        settings.bendSemitones = live.bendSemitones;
	        settings.gate = live.gate;
	        asHarm(storage).setTarget(settings);
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        // ONE PITCH, BEND ALREADY FOLDED IN — registry.h's contract. `bendSemitones` is zeroed
	        // rather than left alone precisely so it cannot be added twice: a caller on this road has
	        // already summed it, and a stale value from a direct caller would detune the oscillator.
	        Settings settings = asHarm(storage).target();
	        settings.noteSemitones = note.pitchSemitones;
	        settings.bendSemitones = 0.0f;
	        settings.gate = note.gate;
	        asHarm(storage).setTarget(settings);
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asHarm(storage).processStereo(left, right, frames);
        },
    // DECLARED RATHER THAN LEFT TO THE NULL-RIGHT ACCIDENT. Harm::processStereo happens to tolerate a
    // null right — it is the stereo loop with nowhere to put the spread — but a host may not rely on
    // that, because the device beside this one in the registry would crash on the same call.
    .processMono =
        [](void* storage, float* mono, int frames) { asHarm(storage).processMono(mono, frames); },
    // LEVEL, PHASE AND SPREAD ARE READ PER SAMPLE WHILE THEY MOVE (backlog FK), in the voices and on the bus alike.
    .windowedControls = kWindowedControls,
    .setFieldWindows =
        [](void* storage, const float* const* runs, size_t count, int) {
	        const auto at = [&](Param p) { return static_cast<size_t>(p) < count ? runs[static_cast<size_t>(p)] : nullptr; };
	        for (int k = 0; k < kPartialCount; ++k) {
		        asHarm(storage).setWindowRuns(k, at(partialParam(k, PartialField::Level)),
		                                      at(partialParam(k, PartialField::Phase)),
		                                      at(partialParam(k, PartialField::Spread)));
	        }
        },
};

} // namespace feathers::fx::harm
