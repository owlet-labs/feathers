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

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// HARM'S ENVELOPE: A DAHDSR WHOSE ATTACK, DECAY AND RELEASE ARE THE ONE-POLE TIME CONSTANTS THE DEVICE ALWAYS HAD.
//
// WHY IT IS NOT THE MODULATOR'S ENVELOPE, and what it does share. The DAHDSR modulator (mod/dahdsr) runs each timed stage as a
// fixed-length shaped ramp. HARM's attack and release have never been that: each is a one-pole whose time is its time
// CONSTANT, which never quite arrives, and every saved patch and factory preset depends on that curve. Swapping in
// fixed-length ramps would change how every existing HARM sounds, so the A and R keep their law, and the stages the
// modulator has and HARM lacked (delay, hold, decay, sustain) are added around them. What is shared is the time law —
// log2 seconds with the bottom of the travel meaning "this stage does not happen" — and the way the stages are drawn.
//
// THE DEFAULTS ARE THE OLD ENVELOPE, TO THE BIT. With no delay, no hold, decay off and sustain at 1, a note's attack runs
// until the envelope is within `kDone` of the peak and then simply carries on towards the same peak with the same
// coefficient, which is the arithmetic the attack-and-release envelope was. Nothing is evaluated differently; a stage
// boundary that changes nothing is only bookkeeping.
//
// A STAGE ENDS WHEN THE ENVELOPE IS WITHIN `kDone` (0.1 %, -60 dB) OF WHERE IT IS GOING, because a one-pole never arrives.
// That is the moment the next stage takes over: hold starts counting, a decay with nothing to fall to is skipped.

namespace feathers::fx::harm {

/// How close to its destination a one-pole stage counts as having arrived, as a fraction of the distance it had to cover.
inline constexpr double kEnvelopeDone = 1.0e-3;

/// The coefficients and counts one chunk of samples runs on — what `Harm` derives once per call from the settings.
struct EnvelopeCoefs {
	double attack = 0.0;  ///< one-pole retention per sample; 0 snaps
	double decay = 0.0;
	double release = 0.0;
	int delaySamples = 0;
	int holdSamples = 0;
	double sustain = 1.0;
};

class Envelope {
public:
	enum class Stage : uint8_t { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

	/// The firmware's `kHarmEnvThreshold`: below this a releasing envelope is zero (see Harm::kEnvelopeFloor).
	static constexpr double kFloor = 1.0e-7;

	void reset() noexcept {
		level_ = 0.0;
		stage_ = Stage::Idle;
		count_ = 0;
		gateWas_ = false;
	}
	double level() const noexcept { return level_; }
	Stage stage() const noexcept { return stage_; }

	/// One sample on. `gate` is the note being held.
	double step(bool gate, const EnvelopeCoefs& c) noexcept {
		if (gate && !gateWas_) {
			stage_ = c.delaySamples > 0 ? Stage::Delay : Stage::Attack;
			count_ = 0;
		}
		else if (!gate && gateWas_) {
			stage_ = Stage::Release;
		}
		gateWas_ = gate;

		if (!gate) {
			// THE OLD RELEASE, verbatim: toward zero by the release coefficient, floored so a one-pole that never arrives
			// does not leave the sine's gain denormal after every note.
			level_ = c.release == 0.0 ? 0.0 : 0.0 + (level_ - 0.0) * c.release;
			if (level_ < kFloor) {
				level_ = 0.0;
				stage_ = Stage::Idle;
			}
			else {
				stage_ = Stage::Release;
			}
			return level_;
		}

		switch (stage_) {
		case Stage::Delay:
			// THE LEVEL HOLDS WHERE IT STANDS: a retrigger during a tail does not drop to zero while it waits.
			if (++count_ >= c.delaySamples) {
				stage_ = Stage::Attack;
				count_ = 0;
			}
			break;
		case Stage::Attack:
			level_ = approach(1.0, c.attack);
			if (1.0 - level_ <= kEnvelopeDone) {
				count_ = 0;
				stage_ = c.holdSamples > 0 ? Stage::Hold : afterPeak(c);
			}
			break;
		case Stage::Hold:
			level_ = approach(1.0, c.attack);
			if (++count_ >= c.holdSamples) {
				stage_ = afterPeak(c);
			}
			break;
		case Stage::Decay:
			level_ = approach(c.sustain, c.decay);
			if (level_ - c.sustain <= kEnvelopeDone * (1.0 - c.sustain)) {
				stage_ = Stage::Sustain;
			}
			break;
		case Stage::Idle:
		case Stage::Release:
			// A gate that is up and a stage that says otherwise (a reset under a held note): start the note.
			stage_ = c.delaySamples > 0 ? Stage::Delay : Stage::Attack;
			count_ = 0;
			break;
		case Stage::Sustain:
			// FOLLOWS THE SUSTAIN LEVEL, rising by the attack's law and falling by the decay's. At a full sustain this is
			// the attack carrying on towards the peak, which is the old envelope.
			level_ = approach(c.sustain, c.sustain >= level_ ? c.attack : c.decay);
			break;
		}
		return level_;
	}

private:
	double approach(double destination, double coefficient) const noexcept {
		return coefficient == 0.0 ? destination : destination + (level_ - destination) * coefficient;
	}
	/// WHERE THE PEAK GOES NEXT: down to the sustain, by the decay if it has one, and nowhere if the sustain is the peak.
	Stage afterPeak(const EnvelopeCoefs& c) noexcept {
		if (c.sustain >= 1.0) {
			return Stage::Sustain;
		}
		if (c.decay == 0.0) {
			level_ = c.sustain; // a decay that does not happen reaches its end at once
			return Stage::Sustain;
		}
		return Stage::Decay;
	}

	double level_ = 0.0;
	Stage stage_ = Stage::Idle;
	int count_ = 0;
	bool gateWas_ = false;
};

/// WHAT THE ENVELOPE'S SETTINGS ARE, in seconds and a level — the window and the tests ask for the shape from these.
struct EnvelopeTimes {
	double delay = 0.0;
	double attack = 0.0; ///< a time CONSTANT, as the control has always been
	double hold = 0.0;
	double decay = 0.0;
	double sustain = 1.0;
	double release = 1.0;
};

/// ONE STAGE OF THE DRAWN SHAPE.
struct EnvelopeSegment {
	Envelope::Stage stage = Envelope::Stage::Idle;
	double seconds = 0.0; ///< the real time this stage takes in the sound
	double from = 0.0;    ///< the level it starts at
	double to = 0.0;      ///< and the level it lands on
};

/// THE ENVELOPE OF ONE NOTE, stage by stage, as `Envelope` plays it with the gate held through the sustain and then let go.
/// Every stage that happens appears; a stage the settings skip does not. Durations are the real ones: a one-pole stage
/// takes `-ln(kEnvelopeDone)` time constants to be done, which is when the envelope hands over to the next.
inline std::vector<EnvelopeSegment> envelopeSegments(const EnvelopeTimes& t) {
	using S = Envelope::Stage;
	const double settle = -std::log(kEnvelopeDone);
	std::vector<EnvelopeSegment> out;
	if (t.delay > 0.0) {
		out.push_back({ S::Delay, t.delay, 0.0, 0.0 });
	}
	out.push_back({ S::Attack, t.attack > 0.0 ? t.attack * settle : 0.0, 0.0, 1.0 });
	if (t.hold > 0.0) {
		out.push_back({ S::Hold, t.hold, 1.0, 1.0 });
	}
	if (t.sustain < 1.0 && t.decay > 0.0) {
		out.push_back({ S::Decay, t.decay * settle, 1.0, t.sustain });
	}
	out.push_back({ S::Sustain, 0.0, t.sustain, t.sustain });
	out.push_back({ S::Release, t.release > 0.0 ? t.release * settle : 0.0, t.sustain, 0.0 });
	return out;
}

/// THE LEVEL `segment` HAS REACHED `seconds` INTO IT — the closed form of what `Envelope::step` does sample by sample.
inline double envelopeLevelAt(const EnvelopeSegment& segment, const EnvelopeTimes& t, double seconds) {
	using S = Envelope::Stage;
	const auto pole = [](double from, double to, double tau, double at) {
		return tau > 0.0 ? to + (from - to) * std::exp(-at / tau) : to;
	};
	switch (segment.stage) {
	case S::Attack:
		return pole(segment.from, 1.0, t.attack, seconds);
	case S::Decay:
		return pole(1.0, t.sustain, t.decay, seconds);
	case S::Release:
		return pole(segment.from, 0.0, t.release, seconds);
	case S::Delay:
	case S::Hold:
	case S::Sustain:
	case S::Idle:
		return segment.from;
	}
	return segment.from;
}

} // namespace feathers::fx::harm
