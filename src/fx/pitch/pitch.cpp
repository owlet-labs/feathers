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

#include "fx/pitch/pitch.h"

#include "fx/ring_lerp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::pitch {

namespace {

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= std::size(kParamDescs)) {
		return nullptr;
	}
	return &kParamDescs[index];
}

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	// STEPPED CONTROLS ROUND ON THE WAY IN — device.h's rule; this decode is one of the readers.
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

/// THE FEWEST FRAMES A GRAIN MAY BE, whatever the sample rate and Size say — below this the phase
/// increment stops being small against one and the window stops being a window.
constexpr double kMinGrainFrames = 16.0;

/// THE FEEDBACK LOOP'S SAFETY NET — the delay's, for the delay's reason: IDENTITY below full scale,
/// so it is a net and not a tone control, then asymptotic to 2.
float softClip(float x) noexcept {
	const float a = std::abs(x);
	if (a <= 1.0f) {
		return x;
	}
	return (x < 0.0f ? -1.0f : 1.0f) * (2.0f - 1.0f / a);
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Scale)] = static_cast<float>(s.scale);
	row[static_cast<size_t>(Param::Shift)] = s.shiftSemitones;
	row[static_cast<size_t>(Param::Fine)] = s.fineCents;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::Mode)] = static_cast<float>(static_cast<int>(s.mode));
	row[static_cast<size_t>(Param::Size)] = s.sizeMs;
	row[static_cast<size_t>(Param::Jitter)] = s.jitter;
	row[static_cast<size_t>(Param::Width)] = s.width;
	row[static_cast<size_t>(Param::Feedback)] = s.feedback;
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		// A FIELD THE PAYLOAD DOES NOT REACH KEEPS ITS DECLARED DEFAULT — device_row.h's rule.
		return static_cast<size_t>(p) < fieldsPresent ? coerce(*d, row[static_cast<size_t>(p)])
		                                              : static_cast<float>(d->def);
	};

	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.scale = static_cast<size_t>(std::lround(read(Param::Scale)));
	s.shiftSemitones = read(Param::Shift);
	s.fineCents = read(Param::Fine);
	s.mix = read(Param::Mix);
	s.mode = static_cast<Mode>(std::lround(read(Param::Mode)));
	s.sizeMs = read(Param::Size);
	s.jitter = read(Param::Jitter);
	s.width = read(Param::Width);
	s.feedback = read(Param::Feedback);
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
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
	// THROUGH THE ROW, so there is one field order and one coercion rule.
	Row row = rowOf(*state);
	row[index] = coerce(*desc, value);
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
}

double ratioFor(const Settings& s, bool right) noexcept {
	// **THE ORDER IS THE MEANING**: the snap acts on the Shift knob ALONE, and Fine and Width are
	// added after it — so a locked interval can still be detuned, and Width still widens it.
	// Snapping the sum instead would swallow both controls whole whenever a Scale is chosen, which
	// is two knobs on the face that do nothing: the honesty rule's exact subject.
	const double snapped = snapSemitones(static_cast<double>(s.shiftSemitones), s.scale);
	const double widthCents = 0.5 * kWidthCentsMax * static_cast<double>(s.width) * (right ? 1.0 : -1.0);
	const double semitones = snapped + (static_cast<double>(s.fineCents) + widthCents) / 100.0;
	return std::exp2(semitones / 12.0);
}

//--------------------------------------------------------------------------------------------------//
// THE SHIFTER.                                                                                       //
//--------------------------------------------------------------------------------------------------//

void PitchShifter::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE ONE ALLOCATION, sized from a constant rather than from the settings — a setting moves on
	// the audio thread and an allocation may not follow it there.
	line_.prepare(sampleRate_, kLineSeconds);
	reset();
}

void PitchShifter::reset() noexcept {
	line_.reset();
	writeCounter_ = 0;
	phase_ = 0.0;
	for (auto& channel : delay_) {
		channel[0] = 0.0;
		channel[1] = 0.0;
	}
	seated_ = false;
	loopL_ = 0.0f;
	loopR_ = 0.0f;
	rng_ = kRngSeed;
	lastRatioL_ = 1.0;
	lastRatioR_ = 1.0;
	reseats_ = 0;
}

void PitchShifter::setTarget(const Settings& settings) noexcept { target_ = settings; }

float PitchShifter::nextUnit() noexcept {
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	// THE TOP 24 BITS, so the result is exact in a float and strictly below one.
	return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

void PitchShifter::seatHead(int head, double ratioLeft, double ratioRight, double grainFrames,
                            bool reverse, double jitter, double loopFrames) noexcept {
	// DRAWN WHETHER OR NOT JITTER IS UP, so the stream's position depends on how many grains have
	// played and not on where a knob has been — turning Jitter up and back down leaves the device
	// in the state it would have been in anyway.
	const double scatter = jitter * static_cast<double>(nextUnit()) * grainFrames;
	const auto seat = [&](double ratio) {
		const double lead = reverse ? 0.0 : std::max(0.0, ratio - 1.0) * grainFrames;
		return lead + scatter + loopFrames;
	};
	delay_[0][head] = seat(ratioLeft);
	delay_[1][head] = seat(ratioRight);
	++reseats_;
}

void PitchShifter::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0) {
		return;
	}
	const Settings s = target_;
	if (!s.enabled) {
		// THE STATE IS CLEARED ONCE, ON THE WAY OUT: the line, the heads' delays, the grain phase and
		// the feedback loop would otherwise resume from the last enabled sample, and a re-enable
		// would play the audio from before the bypass.
		if (running_) {
			running_ = false;
			reset();
		}
		return;
	}
	running_ = true;
	const auto capFrames = static_cast<double>(line_.length());
	if (capFrames < 4.0) {
		return;
	}

	// RESOLVED ONCE PER BLOCK — the controls themselves only move once per `device::kTickWindow`.
	const double ratioL = ratioFor(s, false);
	const double ratioR = ratioFor(s, true);
	lastRatioL_ = ratioL;
	lastRatioR_ = ratioR;

	const bool reverse = s.mode == Mode::Reverse;
	const double fade = fadeFor(s.mode);
	const double grainFrames =
	    std::max(kMinGrainFrames, static_cast<double>(s.sizeMs) * 0.001 * sampleRate_);
	const double phaseInc = 1.0 / grainFrames;
	const auto jitter = static_cast<double>(s.jitter);

	// **WHAT A HEAD'S DELAY DOES EVERY SAMPLE.** The line gains a frame; a forward head consumes
	// `ratio` of them, so its delay moves by the difference. A reversed head walks the other way
	// through the material at the same speed, so it falls behind by the SUM.
	const double slopeL = reverse ? 1.0 + ratioL : 1.0 - ratioL;
	const double slopeR = reverse ? 1.0 + ratioR : 1.0 - ratioR;
	const double maxDelay = capFrames - 2.0;

	// FEEDBACK'S GRAIN OF LOOP DELAY — `kFeedbackMax` has the argument. Zero when Feedback is, so
	// the seat is untouched and a shift of zero stays the input exactly.
	const float feedback = s.feedback;
	const double loopFrames = feedback > 0.0f ? grainFrames : 0.0;

	if (!seated_) {
		seatHead(0, ratioL, ratioR, grainFrames, reverse, jitter, loopFrames);
		seatHead(1, ratioL, ratioR, grainFrames, reverse, jitter, loopFrames);
		seated_ = true;
	}

	for (int i = 0; i < frames; ++i) {
		// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 (as Comp and EQ do): written into the line it
		// would be read back for ever, and through the feedback loop it would never leave.
		const float inL = std::isfinite(left[i]) ? left[i] : 0.0f;
		const float inR = std::isfinite(right[i]) ? right[i] : 0.0f;

		// WRITE FIRST, THEN READ — so a delay of zero is THIS sample, and a shift of zero with no
		// jitter returns the input to the bit rather than the input one sample late.
		//
		// **AND THE BRANCH IS NOT AN OPTIMISATION.** `softClip(inL + 0 * loopL_)` is `inL` only
		// while `inL` is inside full scale; a hot input through a Feedback of zero would be clipped
		// by a control that is off.
		if (feedback > 0.0f) {
			line_.write(softClip(inL + feedback * loopL_), softClip(inR + feedback * loopR_));
		}
		else {
			line_.write(inL, inR);
		}
		++writeCounter_;
		const auto newest = static_cast<double>(writeCounter_ - 1u);

		const auto share = static_cast<float>(headZeroShare(phase_, fade));

		float l0 = 0.0f;
		float l1 = 0.0f;
		float r0 = 0.0f;
		float r1 = 0.0f;
		float unused = 0.0f;
		// ONE READ SERVES BOTH CHANNELS WHILE THEIR HEADS COINCIDE, which is whenever Width is at
		// zero and has been for a grain. Compared on the delays themselves rather than on the knob,
		// because a Width that has just come back to zero leaves the heads apart until they re-seat.
		for (int h = 0; h < 2; ++h) {
			float& outL = h == 0 ? l0 : l1;
			float& outR = h == 0 ? r0 : r1;
			if (delay_[0][h] == delay_[1][h]) {
				fx::readLerp(line_, newest - delay_[0][h], &outL, &outR);
			}
			else {
				fx::readLerp(line_, newest - delay_[0][h], &outL, &unused);
				fx::readLerp(line_, newest - delay_[1][h], &unused, &outR);
			}
		}

		// **`b + (a - b) * share`, NOT `a * share + b * (1 - share)`.** When the two heads read the
		// same sample — a shift of zero — the first form is that sample exactly, whatever the share;
		// the second is that sample times two floats that sum to one only approximately.
		const float wetL = l1 + (l0 - l1) * share;
		const float wetR = r1 + (r0 - r1) * share;

		// HELD FOR THE NEXT SAMPLE'S WRITE, and flushed to exact zero at the floor so the loop ends
		// rather than parking in the denormals.
		loopL_ = std::abs(wetL) < kSilenceFloor ? 0.0f : wetL;
		loopR_ = std::abs(wetR) < kSilenceFloor ? 0.0f : wetR;

		// THE WET, AND ONLY THE WET (backlog NR, D1): the Mix is the slot's now (`Traits::mixControl`), which blends
		// what entered it against this — and against whatever a POST chain makes of this. Right then left, so the mono
		// road (one buffer) keeps the left.
		right[i] = wetR;
		left[i] = wetL;

		for (int h = 0; h < 2; ++h) {
			// CLAMPED, NOT WRAPPED. A Shift or Size that moves mid-grain can carry a head to the
			// write position before its window closes; held there it plays unshifted for the
			// remainder of one grain, where wrapping it would read the oldest audio in the line.
			delay_[0][h] = std::clamp(delay_[0][h] + slopeL, 0.0, maxDelay);
			delay_[1][h] = std::clamp(delay_[1][h] + slopeR, 0.0, maxDelay);
		}

		const double before = phase_;
		phase_ += phaseInc;
		if (before < 0.5 && phase_ >= 0.5) {
			seatHead(1, ratioL, ratioR, grainFrames, reverse, jitter, loopFrames);
		}
		if (phase_ >= 1.0) {
			phase_ -= 1.0;
			seatHead(0, ratioL, ratioR, grainFrames, reverse, jitter, loopFrames);
		}
	}
}

void PitchShifter::processMono(float* mono, int frames) noexcept {
	if (mono == nullptr || frames <= 0) {
		return;
	}
	// THE STEREO ROAD OVER ONE BUFFER, and the LEFT channel is what survives — see the store order
	// in `processStereo`. Both channels are READ before either is stored, so the collision is only
	// between the two stores.
	processStereo(mono, mono, frames);
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
PitchShifter& asShifter(void* storage) { return *static_cast<PitchShifter*>(storage); }
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(PitchShifter),
    .stateAlign = alignof(PitchShifter),
    .construct = [](void* storage) { new (storage) PitchShifter(); },
    .destruct = [](void* storage) { asShifter(storage).~PitchShifter(); },
    .prepare = [](void* storage, double sampleRate, int) { asShifter(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asShifter(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asShifter(storage).setTarget(stateOf(row, take).settings);
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asShifter(storage).processStereo(left, right, frames);
        },
    .processMono =
        [](void* storage, float* mono, int frames) { asShifter(storage).processMono(mono, frames); },
    .holdsTail = [](const void* storage) { return static_cast<const PitchShifter*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::pitch
